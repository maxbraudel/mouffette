#ifndef FFMPEGVIDEODECODER_H
#define FFMPEGVIDEODECODER_H

#include <QObject>
#include <QThread>
#include <QMutex>
#include <QWaitCondition>
#include <QImage>
#include <QString>
#include <QTimer>
#include <atomic>
#include <memory>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

/**
 * FFmpeg-based video decoder that runs in a dedicated thread.
 * Provides frame-accurate seeking, playback control, and async frame delivery.
 */
class FFmpegVideoDecoder : public QObject {
    Q_OBJECT

public:
    enum class PlaybackState {
        Stopped,
        Playing,
        Paused
    };

    explicit FFmpegVideoDecoder(QObject* parent = nullptr);
    ~FFmpegVideoDecoder();

    // Thread-safe control methods (can be called from any thread)
    void setSource(const QString& filePath);
    void play();
    void pause();
    void stop();
    void setPosition(qint64 positionMs);
    void setPlaybackRate(double rate);

    // Thread-safe getters
    qint64 duration() const { return m_duration; }
    qint64 position() const { return m_position; }
    PlaybackState playbackState() const { return m_playbackState.load(); }
    bool hasVideo() const { return m_hasVideo; }
    QSize videoSize() const { return m_videoSize; }

    // Move to dedicated thread
    void moveToWorkerThread();
    // Request a single decoded frame (poster) without starting playback
    void requestFirstFrame();

signals:
    // Emitted from worker thread (use Qt::QueuedConnection)
    void frameReady(const QImage& frame, qint64 timestampMs);
    void durationChanged(qint64 durationMs);
    void positionChanged(qint64 positionMs);
    void playbackStateChanged(PlaybackState state);
    void error(const QString& errorString);

public slots:
    // These slots run in the worker thread
    void initializeDecoder();
    void cleanupDecoder();

private slots:
    void processFrame();
    void performPendingSeek();

private:
    // FFmpeg context (only accessed from worker thread)
    AVFormatContext* m_formatContext = nullptr;
    AVCodecContext* m_codecContext = nullptr;
    AVFrame* m_frame = nullptr;
    AVFrame* m_frameRGB = nullptr;
    SwsContext* m_swsContext = nullptr;
    uint8_t* m_buffer = nullptr;
    int m_videoStreamIndex = -1;

    // Playback state (thread-safe)
    mutable QMutex m_stateMutex;
    std::atomic<qint64> m_duration{0};
    std::atomic<qint64> m_position{0};
    std::atomic<PlaybackState> m_playbackState{PlaybackState::Stopped};
    std::atomic<double> m_playbackRate{1.0};
    std::atomic<bool> m_hasVideo{false};
    QSize m_videoSize;
    // Do not emit or report positions earlier than this after a seek; -1 disables the guard
    std::atomic<qint64> m_minPositionAfterSeek{-1};

    // Control commands (thread-safe)
    mutable QMutex m_commandMutex;
    QString m_pendingFilePath;
    std::atomic<bool> m_seekRequested{false};
    std::atomic<qint64> m_seekPosition{0};
    std::atomic<PlaybackState> m_requestedState{PlaybackState::Stopped};

    // Worker thread components
    QThread* m_workerThread = nullptr;
    QTimer* m_playbackTimer = nullptr;
    // Timer to coalesce rapid seek requests (scrubbing)
    QTimer* m_seekCoalesceTimer = nullptr;
    std::atomic<bool> m_shouldStop{false};

    // Frame timing
    qint64 m_lastFrameTime = 0;
    qint64 m_frameInterval = 33; // ~30 FPS default
    // Playback timing reference (system ms and video ms) for accurate synchronization
    qint64 m_playbackStartSystemMs = 0;
    qint64 m_playbackStartVideoMs = 0;
    // One-shot request to decode a single frame for poster/preview without starting playback
    std::atomic<bool> m_serveOneFrame{false};

    // Helper methods (worker thread only)
    bool openFile(const QString& filePath);
    void closeFile();
    bool seekToPosition(qint64 positionMs);
    QImage convertFrameToQImage(AVFrame* frame);
    void updatePlaybackState(PlaybackState newState);
    qint64 getFrameTimestampMs(AVFrame* frame);
};

#endif // FFMPEGVIDEODECODER_H
