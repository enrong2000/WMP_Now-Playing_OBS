#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace obs_wmp {

enum class PlaybackStatus {
	unknown,
	closed,
	opened,
	changing,
	stopped,
	playing,
	paused,
};

struct PlaylistItem {
	int index = -1;
	std::string title;
	std::string artist;
	std::string album;
	double duration_sec = 0.0;
};

struct MediaState {
	bool available = false;
	bool timeline_available = false;
	bool legacy_wmp_running = false;
	std::string error_message;
	std::string backend;
	std::string source_app_id;
	std::string title;
	std::string artist;
	std::string album;
	std::string album_artist;
	std::string composer;
	PlaybackStatus playback_status = PlaybackStatus::unknown;
	int64_t start_ms = 0;
	int64_t end_ms = 0;
	int64_t position_ms = 0;
	std::chrono::steady_clock::time_point captured_at =
		std::chrono::steady_clock::now();

	/* Playlist data */
	std::vector<PlaylistItem> playlist;
	int current_playlist_index = -1;
};

class SmtcMonitor {
public:
	SmtcMonitor() = default;
	~SmtcMonitor();

	SmtcMonitor(const SmtcMonitor &) = delete;
	SmtcMonitor &operator=(const SmtcMonitor &) = delete;

	void start();
	void stop();
	void configure(std::string app_filter, uint32_t refresh_ms);
	MediaState snapshot() const;

private:
	void run();

	mutable std::mutex mutex_;
	std::condition_variable wake_;
	std::thread worker_;
	bool stop_requested_ = false;
	bool started_ = false;
	std::string app_filter_ = "wmplayer";
	uint32_t refresh_ms_ = 1000;
	MediaState state_;
};

const char *playback_status_text(PlaybackStatus status);

} // namespace obs_wmp
