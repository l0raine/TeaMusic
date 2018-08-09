#include <providers/yt/YoutubeMusicPlayer.h>
#include <misc/pstream.h>
#include <json/json.h>
#include "YTVManager.h"

using namespace std;
using namespace yt;
using namespace sql;
using namespace music;

static const char* yt_command = "youtube-dl -v --no-check-certificate -s --print-json --get-thumbnail %s";

YTVManager::YTVManager(sql::SqlManager* handle) {
    this->sql = handle;
    this->root = fs::u8path("yt");
}
YTVManager::~YTVManager() {}

static const char* audio_prefer_codec_queue[] = {"opus", "vorbis", "mp4a.40.2", "none", nullptr};

struct FMTInfo {
    string codec;
    int bitrate;
    string url;
};

#define YTDL_DEBUG_PREFIX "[debug] "
threads::Future<std::shared_ptr<AudioInfo>> YTVManager::downloadAudio(std::string video) {
    threads::Future<std::shared_ptr<AudioInfo>> future;

    _threads.execute([future, video](){
	    /* Execute the command */
        auto cmdBufferLength = strlen(yt_command) + video.length();
        char cmdBuffer[cmdBufferLength];
        sprintf(cmdBuffer, yt_command, video.c_str());
        auto command = string(cmdBuffer);

        music::log::log(music::log::debug, "[YT-DL] Command: " + command);

        redi::pstream proc;
        proc.open(command, redi::pstreams::pstdout | redi::pstreams::pstderr | redi::pstreams::pstdin);
        string response;
        string err;
        size_t bufferLength = 512;
        char buffer[bufferLength];
        string part;
        while(!proc.rdbuf()->exited()) {
            usleep(10);
            while(proc.out().rdbuf()->in_avail() > 0){
                auto read = proc.out().readsome(buffer, bufferLength);
                if(read > 0) response += string(buffer, read);
            }

            while(proc.err().rdbuf()->in_avail() > 0){
                auto read = proc.err().readsome(buffer, bufferLength);
                if(read > 0) err += string(buffer, read);
            }
        }

	    /* Parsing the response */
        vector<string> available_lines;
	    {   //Parse the lines
		    size_t index = 0;
		    do {
			    auto found = response.find('\n', index);
			    available_lines.push_back(response.substr(index, found - index));
			    if(available_lines.back().find_first_not_of(" \n\r") == std::string::npos) available_lines.pop_back();
			    index = found + 1;
		    } while(index != 0);
	    }

	    vector<string> available_error_lines;
	    {   //Parse the lines
		    size_t index = 0;
		    do {
			    auto found = err.find('\n', index);
			    available_error_lines.push_back(err.substr(index, found - index));
			    if(available_error_lines.back().find_first_not_of(" \n\r") == std::string::npos) available_error_lines.pop_back();
			    index = found + 1;
		    } while(index != 0);
	    }

	    /* Analyzing the response */
	    bool debug_notified = false;
	    for(const auto& entry : available_error_lines) {
		    if(entry.find(YTDL_DEBUG_PREFIX) == 0) {
			    if(!debug_notified) {
				    debug_notified = true;
				    log::log(log::trace, "[YT-DL] Got command execution debug:");
			    }
			    log::log(log::trace, "[YT-DL] " + entry);
		    }
	    }

	    for(const auto& error : available_error_lines)
		    if(error.find("ERROR") != std::string::npos) {
				future.executionFailed(error);
			    return;
		    }

	    if(available_lines.size() < 2) {
		    log::log(log::err, "[YT-DL] Malformed response (response to small!)");
		    log::log(log::debug, "[YT-DL] Response:");
		    for(const auto& entry : available_lines)
			    log::log(log::debug, "[YT-DL] " + entry);
		    future.executionFailed("Malformed response (to small)");
		    return;
	    }
        log::log(log::trace, "[YT-DL] Got thumbnail response: " + available_lines[available_lines.size() - 2]);
	    log::log(log::trace, "[YT-DL] Got json response: " + available_lines[available_lines.size() - 1]);
        auto thumbnail = available_lines[available_lines.size() - 2];

        Json::Value root;
        Json::CharReaderBuilder rbuilder;
        std::string errs;

        istringstream jsonStream(available_lines[available_lines.size() - 1]);
        bool parsingSuccessful = Json::parseFromStream(rbuilder, jsonStream, &root, &errs);
        if (!parsingSuccessful)
        {
            future.executionFailed("Failed to parse yt json response. (" + errs + ")");
            return;
        }

        auto stream = !root["is_live"].isNull() && root["is_live"].asBool();
        log::log(log::debug, "[YT-DL] Song title: " + root["fulltitle"].asString());
        log::log(log::debug, "[YT-DL] Song id: " + root["id"].asString());
        log::log(log::debug, string() + "[YT-DL] Live stream: " + (stream ? "yes" : "no"));
        //is_live

        auto requests = root["formats"];
        log::log(log::debug, "Request count: " + to_string(requests.size()));

        vector<FMTInfo> urls;
        for (auto request : requests) {
            auto fmt = request["format"].asString();
            int rate = request["abr"].asInt();

            if(stream) {
                if(fmt.find("HLS") == std::string::npos) continue;
            } else {
                if(fmt.find("audio only") == std::string::npos) continue;
            }
            urls.push_back(FMTInfo{request["acodec"].asString(), rate, request["url"].asString()});
        }
        if(urls.empty()) {
            future.executionFailed("Failed to get a valid audio stream");
            return;
        }

        int index = -1;
        int abr = -1; //Audio bitrate
        string streamUrl;
        for(const auto& entry : urls) {
            int i = 0;
            while(audio_prefer_codec_queue[i]) {
                if(entry.codec == audio_prefer_codec_queue[i])
                    break;
                i++;
            }
            if(i == sizeof(audio_prefer_codec_queue) / sizeof(*audio_prefer_codec_queue)) {
                log::log(log::err, "[YT-DL] Could not resolve yt audio quality '" + entry.codec + "'");
                i = -2;
            }

            bool use = false;
            use |= index == -1 || abr == -1;
            if(!use) use |= i < index && index != -2;
            if(!use) use |= abr < entry.bitrate && entry.bitrate != 0;
            if(use) {
                index = i;
                abr = entry.bitrate;
                streamUrl = entry.url;

            }
        }
        if(streamUrl.empty()) {
            log::log(log::err, "[YT-DL] Failed to get a valid audio stream with valid quality!");
            streamUrl = urls[0].url;
        }
        log::log(log::debug, string() + "[YT-DL] Using audio quality " + audio_prefer_codec_queue[index]);
        future.executionSucceed(std::make_shared<AudioInfo>(AudioInfo{root["fulltitle"].asString(), "unknown", thumbnail, streamUrl, stream}));
    });
    return future;
}

threads::Future<std::shared_ptr<music::MusicPlayer>> YTVManager::playAudio(const std::string& video) {
    threads::Future<std::shared_ptr<music::MusicPlayer>> future;

    auto fut = downloadAudio(video);
    fut.waitAndGetLater([future, fut](std::shared_ptr<AudioInfo> audio){
        if(fut.succeeded() && audio) return future.executionSucceed(make_shared<music::player::YoutubeMusicPlayer>(audio));
        else return future.executionFailed(fut.errorMegssage());
    }, nullptr);

    return future;
}
