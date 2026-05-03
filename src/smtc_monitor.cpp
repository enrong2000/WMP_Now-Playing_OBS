#include "smtc_monitor.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <exception>
#include <sstream>
#include <utility>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Media.h>
#include <winrt/base.h>

namespace obs_wmp {
namespace {

using winrt::Windows::Media::Control::
	GlobalSystemMediaTransportControlsSession;
using winrt::Windows::Media::Control::
	GlobalSystemMediaTransportControlsSessionManager;
using winrt::Windows::Media::Control::
	GlobalSystemMediaTransportControlsSessionPlaybackStatus;

std::string lowercase(std::string value)
{
	std::transform(value.begin(), value.end(), value.begin(),
		       [](unsigned char ch) {
			       return static_cast<char>(std::tolower(ch));
		       });
	return value;
}

int64_t to_ms(winrt::Windows::Foundation::TimeSpan value)
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(value)
		.count();
}

PlaybackStatus to_playback_status(
	GlobalSystemMediaTransportControlsSessionPlaybackStatus status)
{
	switch (status) {
	case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Closed:
		return PlaybackStatus::closed;
	case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Opened:
		return PlaybackStatus::opened;
	case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Changing:
		return PlaybackStatus::changing;
	case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Stopped:
		return PlaybackStatus::stopped;
	case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing:
		return PlaybackStatus::playing;
	case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Paused:
		return PlaybackStatus::paused;
	default:
		return PlaybackStatus::unknown;
	}
}

std::string join_hstrings(
	const winrt::Windows::Foundation::Collections::IVectorView<
		winrt::hstring> &values)
{
	std::ostringstream stream;
	bool first = true;

	for (const auto &value : values) {
		if (!first)
			stream << ", ";
		first = false;
		stream << winrt::to_string(value);
	}

	return stream.str();
}

GlobalSystemMediaTransportControlsSession pick_session(
	const GlobalSystemMediaTransportControlsSessionManager &manager,
	const std::string &filter, std::vector<std::string> &session_ids)
{
	const auto sessions = manager.GetSessions();
	const auto lowered_filter = lowercase(filter);

	GlobalSystemMediaTransportControlsSession fallback{nullptr};

	for (const auto &session : sessions) {
		const auto id = winrt::to_string(session.SourceAppUserModelId());
		session_ids.push_back(id);

		if (!fallback)
			fallback = session;

		if (!lowered_filter.empty() &&
		    lowercase(id).find(lowered_filter) != std::string::npos)
			return session;
	}

	if (lowered_filter.empty()) {
		if (const auto current = manager.GetCurrentSession())
			return current;
	}

	return lowered_filter.empty() ? fallback : nullptr;
}

MediaState unavailable_state(std::string message,
			     std::vector<std::string> session_ids = {})
{
	MediaState state;
	state.available = false;
	state.error_message = std::move(message);
	state.active_sessions = std::move(session_ids);
	state.captured_at = std::chrono::steady_clock::now();
	return state;
}

MediaState capture_state(
	const GlobalSystemMediaTransportControlsSessionManager &manager,
	const std::string &filter)
{
	std::vector<std::string> session_ids;
	const auto session = pick_session(manager, filter, session_ids);

	if (!session)
		return unavailable_state("No matching SMTC media session",
					 std::move(session_ids));

	MediaState state;
	state.available = true;
	state.source_app_id = winrt::to_string(session.SourceAppUserModelId());
	state.active_sessions = std::move(session_ids);
	state.captured_at = std::chrono::steady_clock::now();

	const auto props = session.TryGetMediaPropertiesAsync().get();
	state.title = winrt::to_string(props.Title());
	state.artist = winrt::to_string(props.Artist());
	state.album = winrt::to_string(props.AlbumTitle());
	state.album_artist = winrt::to_string(props.AlbumArtist());
	state.subtitle = winrt::to_string(props.Subtitle());

	const auto joined_genres = join_hstrings(props.Genres());
	if (!joined_genres.empty())
		state.genres.push_back(joined_genres);

	const auto timeline = session.GetTimelineProperties();
	state.start_ms = to_ms(timeline.StartTime());
	state.end_ms = to_ms(timeline.EndTime());
	state.min_seek_ms = to_ms(timeline.MinSeekTime());
	state.max_seek_ms = to_ms(timeline.MaxSeekTime());
	state.position_ms = to_ms(timeline.Position());

	const auto playback_info = session.GetPlaybackInfo();
	state.playback_status =
		to_playback_status(playback_info.PlaybackStatus());

	if (const auto controls = playback_info.Controls()) {
		state.can_play = controls.IsPlayEnabled();
		state.can_pause = controls.IsPauseEnabled();
		state.can_next = controls.IsNextEnabled();
		state.can_previous = controls.IsPreviousEnabled();
		state.can_seek = controls.IsPlaybackPositionEnabled();
	}

	return state;
}

} // namespace

SmtcMonitor::~SmtcMonitor()
{
	stop();
}

void SmtcMonitor::start()
{
	std::lock_guard lock(mutex_);
	if (started_)
		return;

	stop_requested_ = false;
	started_ = true;
	worker_ = std::thread(&SmtcMonitor::run, this);
}

void SmtcMonitor::stop()
{
	{
		std::lock_guard lock(mutex_);
		if (!started_)
			return;
		stop_requested_ = true;
	}

	wake_.notify_all();

	if (worker_.joinable())
		worker_.join();

	std::lock_guard lock(mutex_);
	started_ = false;
}

void SmtcMonitor::configure(std::string app_filter, uint32_t refresh_ms)
{
	{
		std::lock_guard lock(mutex_);
		app_filter_ = std::move(app_filter);
		refresh_ms_ = std::clamp<uint32_t>(refresh_ms, 250, 5000);
	}

	wake_.notify_all();
}

MediaState SmtcMonitor::snapshot() const
{
	std::lock_guard lock(mutex_);
	return state_;
}

void SmtcMonitor::run()
{
	winrt::init_apartment(winrt::apartment_type::multi_threaded);

	GlobalSystemMediaTransportControlsSessionManager manager{nullptr};

	for (;;) {
		std::string filter;
		uint32_t refresh_ms = 1000;

		{
			std::lock_guard lock(mutex_);
			if (stop_requested_)
				break;
			filter = app_filter_;
			refresh_ms = refresh_ms_;
		}

		MediaState next_state;

		try {
			if (!manager) {
				manager =
					GlobalSystemMediaTransportControlsSessionManager::
						RequestAsync()
							.get();
			}

			next_state = capture_state(manager, filter);
		} catch (const winrt::hresult_error &err) {
			manager = nullptr;
			next_state = unavailable_state(winrt::to_string(err.message()));
		} catch (const std::exception &err) {
			manager = nullptr;
			next_state = unavailable_state(err.what());
		} catch (...) {
			manager = nullptr;
			next_state = unavailable_state("Unknown SMTC error");
		}

		{
			std::lock_guard lock(mutex_);
			state_ = std::move(next_state);
		}

		std::unique_lock lock(mutex_);
		wake_.wait_for(lock, std::chrono::milliseconds(refresh_ms),
			       [this] { return stop_requested_; });
		if (stop_requested_)
			break;
	}

	winrt::uninit_apartment();
}

const char *playback_status_text(PlaybackStatus status)
{
	switch (status) {
	case PlaybackStatus::closed:
		return "Closed";
	case PlaybackStatus::opened:
		return "Opened";
	case PlaybackStatus::changing:
		return "Changing";
	case PlaybackStatus::stopped:
		return "Stopped";
	case PlaybackStatus::playing:
		return "Playing";
	case PlaybackStatus::paused:
		return "Paused";
	case PlaybackStatus::unknown:
	default:
		return "Unknown";
	}
}

} // namespace obs_wmp
