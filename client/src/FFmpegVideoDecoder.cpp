#include "FFmpegVideoDecoder.h"
#include <QDebug>
#include <QThread>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QDateTime>
#include <mutex>

FFmpegVideoDecoder::FFmpegVideoDecoder(QObject* parent)
    : QObject(parent)
{
    // Initialize FFmpeg (thread-safe after FFmpeg 4.0)
    static std::once_flag ffmpegInit;
    std::call_once(ffmpegInit, []() {
        qDebug() << "Initializing FFmpeg libraries";
        // Most initialization is automatic in modern FFmpeg
    });
}

FFmpegVideoDecoder::~FFmpegVideoDecoder()
{
    // Signal worker thread to stop
    m_shouldStop = true;
    
    if (m_workerThread && m_workerThread->isRunning()) {
        m_workerThread->quit();
        m_workerThread->wait(3000); // 3 second timeout
        if (m_workerThread->isRunning()) {
            m_workerThread->terminate();
            m_workerThread->wait(1000);
        }
    }
    
    cleanupDecoder();
}

void FFmpegVideoDecoder::moveToWorkerThread()
{
    if (m_workerThread) {
        qWarning() << "FFmpegVideoDecoder already moved to worker thread";
        return;
    }
    
    m_workerThread = new QThread();
    m_workerThread->setObjectName("FFmpegDecoder");
    
    // Move this object to the worker thread
    this->moveToThread(m_workerThread);
    
    // Connect thread signals
    connect(m_workerThread, &QThread::started, this, &FFmpegVideoDecoder::initializeDecoder);
    connect(m_workerThread, &QThread::finished, this, &FFmpegVideoDecoder::cleanupDecoder);
    
    // Start the worker thread
    m_workerThread->start();
}

void FFmpegVideoDecoder::requestFirstFrame()
{
    // Thread-safe flag to request a single frame to be decoded and emitted
    if (QThread::currentThread() == m_workerThread) {
        m_serveOneFrame.store(true);
        // kick the processing loop
        QMetaObject::invokeMethod(this, &FFmpegVideoDecoder::processFrame, Qt::QueuedConnection);
    } else {
        QMetaObject::invokeMethod(this, [this]() { requestFirstFrame(); }, Qt::QueuedConnection);
    }
}

void FFmpegVideoDecoder::setSource(const QString& filePath)
{
    QMutexLocker locker(&m_commandMutex);
    m_pendingFilePath = filePath;
    
    // If we're in the worker thread, process immediately
    if (QThread::currentThread() == m_workerThread) {
        openFile(filePath);
    } else if (m_workerThread && m_workerThread->isRunning()) {
        // Worker already running: queue the file opening for the worker thread
        QMetaObject::invokeMethod(this, [this, filePath]() {
            openFile(filePath);
        }, Qt::QueuedConnection);
    } else {
        // Worker not running yet: initializeDecoder() will pick up m_pendingFilePath
    }
}

void FFmpegVideoDecoder::play()
{
    m_requestedState = PlaybackState::Playing;
    
    if (QThread::currentThread() == m_workerThread) {
        updatePlaybackState(PlaybackState::Playing);
    } else {
        QMetaObject::invokeMethod(this, [this]() {
            updatePlaybackState(PlaybackState::Playing);
        }, Qt::QueuedConnection);
    }
}

void FFmpegVideoDecoder::pause()
{
    m_requestedState = PlaybackState::Paused;
    
    if (QThread::currentThread() == m_workerThread) {
        updatePlaybackState(PlaybackState::Paused);
    } else {
        QMetaObject::invokeMethod(this, [this]() {
            updatePlaybackState(PlaybackState::Paused);
        }, Qt::QueuedConnection);
    }
}

void FFmpegVideoDecoder::stop()
{
    m_requestedState = PlaybackState::Stopped;
    
    if (QThread::currentThread() == m_workerThread) {
        updatePlaybackState(PlaybackState::Stopped);
        seekToPosition(0);
    } else {
        QMetaObject::invokeMethod(this, [this]() {
            updatePlaybackState(PlaybackState::Stopped);
            seekToPosition(0);
        }, Qt::QueuedConnection);
    }
}

void FFmpegVideoDecoder::setPosition(qint64 positionMs)
{
    m_seekPosition = positionMs;
    m_seekRequested = true;
    
    if (QThread::currentThread() == m_workerThread) {
        seekToPosition(positionMs);
    } else {
        QMetaObject::invokeMethod(this, [this, positionMs]() {
            seekToPosition(positionMs);
        }, Qt::QueuedConnection);
    }
}

void FFmpegVideoDecoder::setPlaybackRate(double rate)
{
    m_playbackRate = rate;
    
    // Update frame interval based on playback rate
    if (rate > 0) {
        m_frameInterval = static_cast<qint64>(33.0 / rate); // Base 30 FPS
    }
    
    if (m_playbackTimer) {
        m_playbackTimer->setInterval(static_cast<int>(m_frameInterval));
    }
}

void FFmpegVideoDecoder::initializeDecoder()
{
    qDebug() << "Initializing FFmpeg decoder in thread:" << QThread::currentThread();
    
    // Create high-precision playback timer in worker thread
    m_playbackTimer = new QTimer(this);
    m_playbackTimer->setTimerType(Qt::PreciseTimer);  // Use precise timing
    connect(m_playbackTimer, &QTimer::timeout, this, &FFmpegVideoDecoder::processFrame);
    
    // Default to 60fps timer for smooth playback, actual frame timing handled in processFrame
    m_frameInterval = 33;  // Will be updated when we open a file
    m_playbackTimer->setInterval(16);  // ~60Hz timer for smooth frame pacing
    
    // Process any pending file
    QMutexLocker locker(&m_commandMutex);
    if (!m_pendingFilePath.isEmpty()) {
        QString filePath = m_pendingFilePath;
        m_pendingFilePath.clear();
        locker.unlock();
        openFile(filePath);
    }
}

void FFmpegVideoDecoder::cleanupDecoder()
{
    qDebug() << "Cleaning up FFmpeg decoder";
    
    if (m_playbackTimer) {
        m_playbackTimer->stop();
        m_playbackTimer->deleteLater();
        m_playbackTimer = nullptr;
    }
    
    closeFile();
}

bool FFmpegVideoDecoder::openFile(const QString& filePath)
{
    qDebug() << "Opening file:" << filePath;
    
    // Close any existing file
    closeFile();
    
    // Open input file
    if (avformat_open_input(&m_formatContext, filePath.toUtf8().constData(), nullptr, nullptr) != 0) {
        emit error(QString("Cannot open file: %1").arg(filePath));
        return false;
    }
    
    // Retrieve stream information
    if (avformat_find_stream_info(m_formatContext, nullptr) < 0) {
        emit error("Cannot find stream information");
        closeFile();
        return false;
    }
    
    // Find video stream
    m_videoStreamIndex = av_find_best_stream(m_formatContext, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (m_videoStreamIndex < 0) {
        emit error("No video stream found");
        closeFile();
        return false;
    }
    
    AVStream* videoStream = m_formatContext->streams[m_videoStreamIndex];
    const AVCodec* codec = avcodec_find_decoder(videoStream->codecpar->codec_id);
    if (!codec) {
        emit error("Unsupported video codec");
        closeFile();
        return false;
    }
    
    // Create codec context
    m_codecContext = avcodec_alloc_context3(codec);
    if (!m_codecContext) {
        emit error("Cannot allocate codec context");
        closeFile();
        return false;
    }
    
    // Copy codec parameters
    if (avcodec_parameters_to_context(m_codecContext, videoStream->codecpar) < 0) {
        emit error("Cannot copy codec parameters");
        closeFile();
        return false;
    }
    
    // Open codec
    if (avcodec_open2(m_codecContext, codec, nullptr) < 0) {
        emit error("Cannot open codec");
        closeFile();
        return false;
    }
    
    // Allocate frames
    m_frame = av_frame_alloc();
    m_frameRGB = av_frame_alloc();
    if (!m_frame || !m_frameRGB) {
        emit error("Cannot allocate frames");
        closeFile();
        return false;
    }
    
    // Set video properties
    m_videoSize = QSize(m_codecContext->width, m_codecContext->height);
    m_hasVideo = true;
    
    // Calculate duration
    if (videoStream->duration != AV_NOPTS_VALUE) {
        m_duration.store(av_rescale_q(videoStream->duration, videoStream->time_base, AV_TIME_BASE_Q) / 1000);
    } else if (m_formatContext->duration != AV_NOPTS_VALUE) {
        m_duration.store(m_formatContext->duration / 1000);
    } else {
        m_duration.store(0); // Unknown duration
    }
    
    // Allocate buffer for RGBA conversion
    int numBytes = av_image_get_buffer_size(AV_PIX_FMT_RGBA, m_codecContext->width, m_codecContext->height, 1);
    m_buffer = static_cast<uint8_t*>(av_malloc(numBytes * sizeof(uint8_t)));
    if (av_image_fill_arrays(m_frameRGB->data, m_frameRGB->linesize, m_buffer, AV_PIX_FMT_RGBA, 
                        m_codecContext->width, m_codecContext->height, 1) < 0) {
        emit error("Failed to setup frame buffers");
        closeFile();
        return false;
    }
    
    // Initialize scaling context with RGBA output for Qt
    m_swsContext = sws_getContext(
        m_codecContext->width, m_codecContext->height, m_codecContext->pix_fmt,
        m_codecContext->width, m_codecContext->height, AV_PIX_FMT_RGBA,
        SWS_BILINEAR | SWS_ACCURATE_RND, nullptr, nullptr, nullptr
    );
    
    if (!m_swsContext) {
        emit error("Cannot initialize scaling context");
        closeFile();
        return false;
    }
    
    // Calculate frame interval from video frame rate
    AVRational frameRate = av_guess_frame_rate(m_formatContext, videoStream, nullptr);
    if (frameRate.num && frameRate.den) {
        double fps = av_q2d(frameRate);
        if (fps > 0) {
            // Calculate precise frame interval
            m_frameInterval = static_cast<qint64>(1000.0 / fps + 0.5);  // Round to nearest ms
            
            // Ensure we have a reasonable interval (between 1ms and 100ms)
            m_frameInterval = std::clamp<qint64>(m_frameInterval, 1, 100);
            
            qDebug() << "Video fps:" << fps << "frame interval:" << m_frameInterval << "ms";
        }
    }
    
    qDebug() << "Successfully opened video:" << m_videoSize << "duration:" << m_duration << "ms"
             << "frame interval:" << m_frameInterval << "ms";
    
    // Emit signals
    emit durationChanged(m_duration.load());
    emit playbackStateChanged(PlaybackState::Stopped);
    
    return true;
}

void FFmpegVideoDecoder::closeFile()
{
    if (m_playbackTimer) {
        m_playbackTimer->stop();
    }
    
    updatePlaybackState(PlaybackState::Stopped);
    
    if (m_swsContext) {
        sws_freeContext(m_swsContext);
        m_swsContext = nullptr;
    }
    
    if (m_buffer) {
        av_free(m_buffer);
        m_buffer = nullptr;
    }
    
    if (m_frame) {
        av_frame_free(&m_frame);
        m_frame = nullptr;
    }
    
    if (m_frameRGB) {
        av_frame_free(&m_frameRGB);
        m_frameRGB = nullptr;
    }
    
    if (m_codecContext) {
        avcodec_free_context(&m_codecContext);
        m_codecContext = nullptr;
    }
    
    if (m_formatContext) {
        avformat_close_input(&m_formatContext);
        m_formatContext = nullptr;
    }
    
    m_videoStreamIndex = -1;
    m_hasVideo = false;
    m_videoSize = QSize();
    m_duration.store(0);
    m_position.store(0);
}

bool FFmpegVideoDecoder::seekToPosition(qint64 positionMs)
{
    if (!m_formatContext || m_videoStreamIndex < 0) {
        return false;
    }
    
    AVStream* videoStream = m_formatContext->streams[m_videoStreamIndex];
    int64_t timestamp = av_rescale_q(positionMs * 1000, AV_TIME_BASE_Q, videoStream->time_base);
    
    if (av_seek_frame(m_formatContext, m_videoStreamIndex, timestamp, AVSEEK_FLAG_BACKWARD) < 0) {
        qWarning() << "Seek failed to position:" << positionMs;
        return false;
    }
    
    // Flush codec buffers
    avcodec_flush_buffers(m_codecContext);
    
    m_position.store(positionMs);
    m_seekRequested = false;
    
    emit positionChanged(m_position);
    // If currently playing, update playback anchors so timing remains correct
    if (m_playbackState.load() == PlaybackState::Playing) {
        m_playbackStartVideoMs = m_position;
        m_playbackStartSystemMs = QDateTime::currentMSecsSinceEpoch();
    }
    return true;
}

void FFmpegVideoDecoder::processFrame()
{
    static int frameCount = 0;
    static qint64 lastFrameTime = 0;
    
    if (m_shouldStop || !m_formatContext || !m_codecContext) {
        qWarning() << "Skip frame processing - invalid state:" 
                  << "shouldStop:" << m_shouldStop
                  << "formatCtx:" << (m_formatContext != nullptr)
                  << "codecCtx:" << (m_codecContext != nullptr);
        return;
    }
    
    // Handle pending seek
    if (m_seekRequested) {
        seekToPosition(m_seekPosition);
        lastFrameTime = 0;  // Reset timing after seek
        return;
    }
    
    // Calculate desired presentation timestamp based on wall clock and playback rate
    qint64 currentTime = QDateTime::currentMSecsSinceEpoch();
    if (m_playbackStartSystemMs == 0) {
        // If not initialized, set start anchors based on current position
        m_playbackStartSystemMs = currentTime;
        m_playbackStartVideoMs = m_position.load();
    }

    qint64 wallElapsed = currentTime - m_playbackStartSystemMs;
    double rate = static_cast<double>(m_playbackRate.load());
    qint64 desiredVideoMs = m_playbackStartVideoMs + static_cast<qint64>(wallElapsed * rate);

    // Prevent tight loops when paused or waiting
    if (desiredVideoMs <= m_position.load()) {
        return;
    }
    
    if (++frameCount % 30 == 0) {
        qDebug() << "Processing frame" << frameCount 
                 << "state:" << static_cast<int>(m_playbackState.load())
                 << "thread:" << QThread::currentThread()
                 << "wallElapsed:" << wallElapsed << "ms"
                 << "frameInterval:" << m_frameInterval << "ms";
    }
    
    // One-shot poster request: if requested, decode and emit a single frame then stop
    if (m_serveOneFrame.load()) {
        // Decode next available frame and emit it, but do not start playback
        bool posterEmitted = false;
        AVPacket* pkt = av_packet_alloc();
        if (!pkt) return;
        while (av_read_frame(m_formatContext, pkt) >= 0) {
            if (pkt->stream_index != m_videoStreamIndex) { av_packet_unref(pkt); continue; }
            if (avcodec_send_packet(m_codecContext, pkt) < 0) { av_packet_unref(pkt); continue; }
            if (avcodec_receive_frame(m_codecContext, m_frame) == 0) {
                QImage image = convertFrameToQImage(m_frame);
                if (!image.isNull()) {
                    qint64 timestamp = getFrameTimestampMs(m_frame);
                    m_position.store(timestamp);
                    emit frameReady(image, timestamp);
                    emit positionChanged(timestamp);
                    posterEmitted = true;
                }
            }
            av_packet_unref(pkt);
            if (posterEmitted) break;
        }
        av_packet_free(&pkt);
        m_serveOneFrame.store(false);
        return;
    }

    // If not playing, skip normal processing
    if (m_playbackState != PlaybackState::Playing) {
        return;
    }

    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        return;
    }
    
    // We'll decode packets until we find a frame with timestamp >= desiredVideoMs
    bool emitted = false;
    int decodeIterations = 0;
    const int maxDecodeIterations = 8; // safety cap per tick to avoid long blocking
    qDebug() << "Desired video ms:" << desiredVideoMs << "current pos:" << m_position.load();
    while (av_read_frame(m_formatContext, packet) >= 0) {
        if (++decodeIterations > maxDecodeIterations) {
            // Avoid spending too long in one tick; we'll continue next tick
            qDebug() << "Decode iteration limit reached:" << decodeIterations;
            break;
        }
        if (packet->stream_index != m_videoStreamIndex) {
            av_packet_unref(packet);
            continue;
        }

        if (avcodec_send_packet(m_codecContext, packet) < 0) {
            av_packet_unref(packet);
            continue;
        }

        while (avcodec_receive_frame(m_codecContext, m_frame) == 0) {
            qint64 timestamp = getFrameTimestampMs(m_frame);
            qDebug() << "Decoded frame ts:" << timestamp << "desired:" << desiredVideoMs;
            // If this frame is in the future, present it and stop decoding further
            if (timestamp >= desiredVideoMs) {
                QImage image = convertFrameToQImage(m_frame);
                    if (!image.isNull()) {
                        m_position.store(timestamp);
                        emit frameReady(image, timestamp);
                        emit positionChanged(timestamp);
                        emitted = true;
                    }
                break; // stop inner receive loop and break to outer
            } else {
                // Older frame; drop
            }
        }

    av_packet_unref(packet);

        if (emitted) break;
    }

    // Free packet buffer allocated earlier
    av_packet_free(&packet);

    if (!emitted) {
        // No frame emitted this tick; if format indicates EOF, stop playback
        if (m_formatContext && m_formatContext->pb && avio_feof(m_formatContext->pb)) {
            qDebug() << "End of file reached, stopping playback";
            updatePlaybackState(PlaybackState::Stopped);
            seekToPosition(0);
        }
    }
}

QImage FFmpegVideoDecoder::convertFrameToQImage(AVFrame* frame)
{
    if (!frame || !m_swsContext || !m_frameRGB || !m_codecContext) {
        qWarning() << "Invalid state for frame conversion";
        return QImage();
    }
    
    // Convert frame to RGBA
    if (sws_scale(m_swsContext, frame->data, frame->linesize, 0, m_codecContext->height,
              m_frameRGB->data, m_frameRGB->linesize) < 0) {
        qWarning() << "Frame conversion failed";
        return QImage();
    }
    
    // Create QImage from RGBA data (Format_RGBA8888 matches FFmpeg's AV_PIX_FMT_RGBA)
    QImage image(m_frameRGB->data[0], m_codecContext->width, m_codecContext->height, 
                 m_frameRGB->linesize[0], QImage::Format_RGBA8888);
    
    // Return a deep copy to avoid issues with FFmpeg buffer reuse
    return image.copy();
}

qint64 FFmpegVideoDecoder::getFrameTimestampMs(AVFrame* frame)
{
    if (!frame || m_videoStreamIndex < 0) {
        return 0;
    }
    
    AVStream* videoStream = m_formatContext->streams[m_videoStreamIndex];
    if (frame->pts != AV_NOPTS_VALUE) {
        return av_rescale_q(frame->pts, videoStream->time_base, AV_TIME_BASE_Q) / 1000;
    }
    
    return 0;
}

void FFmpegVideoDecoder::updatePlaybackState(PlaybackState newState)
{
    if (m_playbackState.load() == newState) {
        return;
    }
    
    PlaybackState oldState = m_playbackState.load();
    m_playbackState.store(newState);
    
    qDebug() << "Playback state changing from" << static_cast<int>(oldState) << "to" << static_cast<int>(newState) 
             << "in thread:" << QThread::currentThread() << "timer:" << m_playbackTimer;
    
    // Control timer based on state
    if (m_playbackTimer) {
        if (newState == PlaybackState::Playing) {
            qint64 now = QDateTime::currentMSecsSinceEpoch();
            // Anchor playback timing to current system time and current video position
            m_playbackStartSystemMs = now;
            m_playbackStartVideoMs = m_position.load();
            m_playbackTimer->setInterval(static_cast<int>(m_frameInterval));
            m_playbackTimer->start();
            
            // Force an immediate frame process
            QMetaObject::invokeMethod(this, &FFmpegVideoDecoder::processFrame, Qt::QueuedConnection);
        } else {
            m_playbackTimer->stop();
            // Reset playback anchors when not playing
            m_playbackStartSystemMs = 0;
            m_playbackStartVideoMs = 0;
        }
    } else {
        qWarning() << "No playback timer available in state change to:" << static_cast<int>(newState);
    }
    
    emit playbackStateChanged(newState);
}

// MOC is handled by CMake AUTOMOC
