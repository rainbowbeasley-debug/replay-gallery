#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/GJGameLevel.hpp>
#include <Geode/modify/LevelInfoLayer.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/ui/Notification.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/utils/file.hpp>
#include <Geode/utils/general.hpp>
#include <Geode/utils/string.hpp>
#include <Geode/utils/web.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

using namespace geode::prelude;

namespace {
    // Keep using the existing Firebase project so previously uploaded
    // public replays and feedback remain available after the rename.
    constexpr std::string_view FIREBASE_DATABASE_URL =
        "https://showcase-reborn-macros-default-rtdb."
        "firebaseio.com";

    constexpr std::string_view FEEDBACK_MOD_VERSION =
        "v1.2.5";

    int g_feedbackLevelID = 0;
    int g_feedbackLevelVersion = 0;

    bool g_feedbackSending = false;

    constexpr std::int64_t FEEDBACK_COOLDOWN_SECONDS =
        60 * 60;

    constexpr char FEEDBACK_LAST_SENT_KEY[] =
        "feedback-last-sent-unix-seconds-v1";

    // Remove every direct child with a matching Geode node ID.
    // Replay Gallery uses this only for its own nodes so it never
    // invalidates pointers owned by another enabled mod.
    void removeChildrenByID(
        CCNode* parent,
        std::string_view nodeID
    ) {
        if (!parent) {
            return;
        }

        auto const id = std::string(nodeID);

        while (
            auto* child =
                parent->getChildByID(id)
        ) {
            child->removeFromParentAndCleanup(
                true
            );
        }
    }

    struct MacroInput {
        // Replay Gallery v2 stores inputs by relative simulation
        // step instead of wall-clock time or level percentage. This
        // keeps the same input order when a speedhack changes how
        // quickly the game runs in real time.
        unsigned int step = 0;

        bool down = false;
        int button = 1;
        bool isPlayer1 = true;
    };

    struct MacroData {
        // New macros use relative simulation steps. Legacy v1 macros
        // used GJGameState::m_currentProgress and are still playable.
        bool usesSimulationSteps = true;

        bool completed = false;

        // Hundredths of one percent: 3500 means 35.00%.
        unsigned int endPercentHundredths = 0;

        // Relative simulation step where the run completed or died.
        unsigned int endStep = 0;

        std::vector<MacroInput> inputs;
    };

    PlayLayer* g_activePlayLayer = nullptr;

    bool g_recording = false;
    bool g_playback = false;
    bool g_injectingInput = false;

    bool g_attemptInvalid = false;
    bool g_attemptFinished = false;

    // True only while Replay Gallery is playing a public
    // macro. It prevents the playback from saving progress,
    // scores, stars, coins, or a legitimate completion.
    bool g_safePlaybackActive = false;

    MacroData g_currentMacro;
    std::size_t g_playbackIndex = 0;

    // Replay Gallery's own per-attempt physics-command counter.
    // Geometry Dash's m_currentStep is not reliable on every setup
    // (it stayed at zero for some players), so v2 macros are timed
    // using calls to processCommands instead. Speedhack changes wall
    // clock speed, but does not change this recorded command order.
    unsigned int g_attemptStep = 0;

    bool g_pendingPlayback = false;

    int g_pendingLevelID = 0;
    int g_pendingLevelVersion = 0;

    MacroData g_pendingMacro;

    int getLevelID(GJGameLevel* level) {
        if (!level) {
            return 0;
        }

        return level->m_levelID.value();
    }

    int getLevelVersion(GJGameLevel* level) {
        if (!level) {
            return 0;
        }

        return level->m_levelVersion;
    }

    std::string macroFilename(
        GJGameLevel* level
    ) {
        return fmt::format(
            "{}-{}.replaygallery",
            getLevelID(level),
            getLevelVersion(level)
        );
    }

    // The public prototype now uses the level ID as its
    // stable key. A level's version can change between the
    // level page, downloading, and PlayLayer initialization.
    std::string localMacroKey(
        GJGameLevel* level
    ) {
        if (!level) {
            return "replay-gallery-macro-v1-invalid";
        }

        return fmt::format(
            "replay-gallery-macro-v1-{}",
            getLevelID(level)
        );
    }

    // Read saves made under the old Showcase Reborn name so
    // recordings can be migrated after the rename.
    std::string legacyLocalMacroKey(
        GJGameLevel* level
    ) {
        if (!level) {
            return "showcase-macro-v6-invalid";
        }

        return fmt::format(
            "showcase-macro-v6-{}",
            getLevelID(level)
        );
    }

    std::string remoteCacheKey(
        int levelID
    );

    std::string remoteCacheKey(
        GJGameLevel* level
    ) {
        if (!level) {
            return "replay-gallery-firebase-cache-v1-invalid";
        }

        return remoteCacheKey(
            getLevelID(level)
        );
    }

    std::string legacyRemoteCacheKey(
        int levelID
    );

    std::string legacyRemoteCacheKey(
        GJGameLevel* level
    ) {
        if (!level) {
            return "showcase-firebase-cache-v2-invalid";
        }

        return legacyRemoteCacheKey(
            getLevelID(level)
        );
    }

    std::string remoteMacroURL(
        int levelID
    );

    std::string remoteMacroURL(
        GJGameLevel* level
    ) {
        return remoteMacroURL(
            getLevelID(level)
        );
    }

    // Compatibility with macros uploaded by the previous
    // level-ID-plus-version build.
    std::string legacyRemoteMacroURL(
        GJGameLevel* level
    ) {
        return fmt::format(
            "{}/macros/{}-{}.json",
            FIREBASE_DATABASE_URL,
            getLevelID(level),
            getLevelVersion(level)
        );
    }

    std::string encodeJSONString(
        std::string_view text
    ) {
        std::string result;

        result.reserve(
            text.size() + 2
        );

        result.push_back('"');

        constexpr char HEX[] =
            "0123456789abcdef";

        for (
            unsigned char character :
            text
        ) {
            switch (character) {
                case '"':
                    result += "\\\"";
                    break;

                case '\\':
                    result += "\\\\";
                    break;

                case '\b':
                    result += "\\b";
                    break;

                case '\f':
                    result += "\\f";
                    break;

                case '\n':
                    result += "\\n";
                    break;

                case '\r':
                    result += "\\r";
                    break;

                case '\t':
                    result += "\\t";
                    break;

                default:
                    if (character < 0x20) {
                        result += "\\u00";
                        result.push_back(
                            HEX[
                                (character >> 4) &
                                0x0f
                            ]
                        );
                        result.push_back(
                            HEX[
                                character &
                                0x0f
                            ]
                        );
                    }
                    else {
                        result.push_back(
                            static_cast<char>(
                                character
                            )
                        );
                    }

                    break;
            }
        }

        result.push_back('"');

        return result;
    }

    int hexValue(char character) {
        if (
            character >= '0' &&
            character <= '9'
        ) {
            return
                character - '0';
        }

        if (
            character >= 'a' &&
            character <= 'f'
        ) {
            return
                10 +
                character - 'a';
        }

        if (
            character >= 'A' &&
            character <= 'F'
        ) {
            return
                10 +
                character - 'A';
        }

        return -1;
    }

    bool decodeJSONString(
        std::string_view json,
        std::string& result
    ) {
        std::size_t start = 0;
        std::size_t end = json.size();

        while (
            start < end &&
            std::isspace(
                static_cast<unsigned char>(
                    json[start]
                )
            )
        ) {
            ++start;
        }

        while (
            end > start &&
            std::isspace(
                static_cast<unsigned char>(
                    json[end - 1]
                )
            )
        ) {
            --end;
        }

        if (
            json.substr(
                start,
                end - start
            ) == "null"
        ) {
            return false;
        }

        if (
            end - start < 2 ||
            json[start] != '"' ||
            json[end - 1] != '"'
        ) {
            return false;
        }

        std::string decoded;

        decoded.reserve(
            end - start - 2
        );

        for (
            std::size_t index =
                start + 1;
            index < end - 1;
            ++index
        ) {
            char character =
                json[index];

            if (character != '\\') {
                decoded.push_back(
                    character
                );

                continue;
            }

            ++index;

            if (index >= end - 1) {
                return false;
            }

            char escaped =
                json[index];

            switch (escaped) {
                case '"':
                    decoded.push_back('"');
                    break;

                case '\\':
                    decoded.push_back('\\');
                    break;

                case '/':
                    decoded.push_back('/');
                    break;

                case 'b':
                    decoded.push_back('\b');
                    break;

                case 'f':
                    decoded.push_back('\f');
                    break;

                case 'n':
                    decoded.push_back('\n');
                    break;

                case 'r':
                    decoded.push_back('\r');
                    break;

                case 't':
                    decoded.push_back('\t');
                    break;

                case 'u': {
                    if (
                        index + 4 >=
                        end
                    ) {
                        return false;
                    }

                    int value = 0;

                    for (
                        int offset = 1;
                        offset <= 4;
                        ++offset
                    ) {
                        int digit =
                            hexValue(
                                json[
                                    index +
                                    offset
                                ]
                            );

                        if (digit < 0) {
                            return false;
                        }

                        value =
                            value * 16 +
                            digit;
                    }

                    index += 4;

                    if (value <= 0x7f) {
                        decoded.push_back(
                            static_cast<char>(
                                value
                            )
                        );
                    }
                    else if (value <= 0x7ff) {
                        decoded.push_back(
                            static_cast<char>(
                                0xc0 |
                                (
                                    value >> 6
                                )
                            )
                        );

                        decoded.push_back(
                            static_cast<char>(
                                0x80 |
                                (
                                    value &
                                    0x3f
                                )
                            )
                        );
                    }
                    else {
                        decoded.push_back(
                            static_cast<char>(
                                0xe0 |
                                (
                                    value >> 12
                                )
                            )
                        );

                        decoded.push_back(
                            static_cast<char>(
                                0x80 |
                                (
                                    value >> 6
                                ) &
                                0x3f
                            )
                        );

                        decoded.push_back(
                            static_cast<char>(
                                0x80 |
                                (
                                    value &
                                    0x3f
                                )
                            )
                        );
                    }

                    break;
                }

                default:
                    return false;
            }
        }

        result =
            std::move(decoded);

        return true;
    }

    unsigned int relativeSimulationStep(
        GJBaseGameLayer* layer
    ) {
        if (
            !layer ||
            !g_activePlayLayer ||
            static_cast<GJBaseGameLayer*>(
                g_activePlayLayer
            ) != layer
        ) {
            return 0;
        }

        return g_attemptStep;
    }

    unsigned int macroPlaybackPosition(
        GJBaseGameLayer* layer,
        MacroData const& macro
    ) {
        if (!layer) {
            return 0;
        }

        if (macro.usesSimulationSteps) {
            return relativeSimulationStep(layer);
        }

        return static_cast<unsigned int>(
            layer->m_gameState
                .m_currentProgress
        );
    }

    unsigned int currentPercentHundredths(
        PlayLayer* layer
    ) {
        if (!layer) {
            return 0;
        }

        float percent =
            layer->getCurrentPercent();

        if (!std::isfinite(percent)) {
            return 0;
        }

        percent = std::clamp(
            percent,
            0.f,
            100.f
        );

        return static_cast<unsigned int>(
            std::lround(percent * 100.f)
        );
    }

    bool isPlayableMacro(
        MacroData const& macro
    ) {
        bool hasResult =
            macro.completed ||
            macro.endPercentHundredths > 0 ||
            macro.endStep > 0 ||
            !macro.inputs.empty();

        if (!hasResult) {
            return false;
        }

        if (!macro.usesSimulationSteps) {
            return !macro.inputs.empty();
        }

        bool hasNonzeroTiming =
            macro.endStep > 0;

        if (!hasNonzeroTiming) {
            hasNonzeroTiming =
                std::any_of(
                    macro.inputs.begin(),
                    macro.inputs.end(),
                    [](MacroInput const& input) {
                        return input.step > 0;
                    }
                );
        }

        // v1.1.0-v1.1.3 accidentally read m_currentStep on setups
        // where it never advanced. Those files contain many inputs
        // all stamped at step zero, so playback releases everything
        // immediately and looks like no macro is running. Ignore that
        // broken timing so a fresh recording can replace it.
        if (
            !hasNonzeroTiming &&
            (
                macro.completed ||
                macro.endPercentHundredths >= 100 ||
                macro.inputs.size() > 4
            )
        ) {
            return false;
        }

        return !macro.inputs.empty() || macro.endStep > 0;
    }

    bool isBetterMacro(
        MacroData const& candidate,
        MacroData const& current
    ) {
        bool candidatePlayable =
            isPlayableMacro(candidate);

        bool currentPlayable =
            isPlayableMacro(current);

        if (candidatePlayable != currentPlayable) {
            return candidatePlayable;
        }

        if (!candidatePlayable) {
            return false;
        }
        if (
            candidate.completed !=
            current.completed
        ) {
            return candidate.completed;
        }

        if (
            candidate.endPercentHundredths !=
            current.endPercentHundredths
        ) {
            return
                candidate.endPercentHundredths >
                current.endPercentHundredths;
        }

        // Percent can be rounded to the same hundredth. In that case,
        // the run that survived for more simulation steps is farther.
        return candidate.endStep > current.endStep;
    }

    std::string formatRunResult(
        MacroData const& macro
    ) {
        if (macro.completed) {
            return "100% COMPLETE";
        }

        unsigned int whole =
            macro.endPercentHundredths / 100;

        unsigned int fraction =
            macro.endPercentHundredths % 100;

        if (fraction == 0) {
            return fmt::format(
                "{}%",
                whole
            );
        }

        if (fraction % 10 == 0) {
            return fmt::format(
                "{}.{}%",
                whole,
                fraction / 10
            );
        }

        return fmt::format(
            "{}.{:02}%",
            whole,
            fraction
        );
    }

    std::string serializeMacro(
        MacroData const& macro
    ) {
        std::ostringstream output;

        output
            << "REPLAY_GALLERY_MACRO_V2\n";

        output
            << (macro.completed ? 1 : 0)
            << "\n";

        output
            << macro.endPercentHundredths
            << "\n";

        output
            << macro.endStep
            << "\n";

        output
            << macro.inputs.size()
            << "\n";

        for (
            MacroInput const& input :
            macro.inputs
        ) {
            output
                << input.step
                << " "
                << (input.down ? 1 : 0)
                << " "
                << input.button
                << " "
                << (input.isPlayer1 ? 1 : 0)
                << "\n";
        }

        return output.str();
    }

    bool deserializeMacro(
        std::string const& text,
        MacroData& result
    ) {
        std::istringstream inputStream(text);

        std::string header;

        if (!std::getline(
            inputStream,
            header
        )) {
            return false;
        }

        if (
            !header.empty() &&
            header.back() == '\r'
        ) {
            header.pop_back();
        }

        bool version2 =
            header ==
                "REPLAY_GALLERY_MACRO_V2";

        bool legacyVersion1 =
            header ==
                "REPLAY_GALLERY_MACRO_V1" ||
            header ==
                "SHOWCASE_REBORN_MACRO_V1";

        if (!version2 && !legacyVersion1) {
            return false;
        }

        MacroData loadedMacro;
        std::size_t inputCount = 0;

        if (version2) {
            int completedValue = 0;

            if (!(
                inputStream >>
                completedValue
            )) {
                return false;
            }

            if (
                completedValue != 0 &&
                completedValue != 1
            ) {
                return false;
            }

            if (!(
                inputStream >>
                loadedMacro
                    .endPercentHundredths
            )) {
                return false;
            }

            if (
                loadedMacro
                    .endPercentHundredths >
                10000
            ) {
                return false;
            }

            if (!(
                inputStream >>
                loadedMacro.endStep
            )) {
                return false;
            }

            if (!(
                inputStream >>
                inputCount
            )) {
                return false;
            }

            loadedMacro.usesSimulationSteps =
                true;

            loadedMacro.completed =
                completedValue == 1;

            if (loadedMacro.completed) {
                loadedMacro
                    .endPercentHundredths =
                    10000;
            }
        }
        else {
            // Version 1 only stored completed runs. Its input
            // timestamps used m_currentProgress, so keep the old
            // timing path when one of those macros is loaded.
            unsigned int legacyEndProgress = 0;

            if (!(
                inputStream >>
                legacyEndProgress
            )) {
                return false;
            }

            if (!(
                inputStream >>
                inputCount
            )) {
                return false;
            }

            loadedMacro.usesSimulationSteps =
                false;

            loadedMacro.completed = true;

            loadedMacro
                .endPercentHundredths =
                10000;

            loadedMacro.endStep =
                legacyEndProgress;
        }

        if (inputCount > 1000000) {
            return false;
        }

        loadedMacro.inputs.reserve(
            inputCount
        );

        for (
            std::size_t index = 0;
            index < inputCount;
            ++index
        ) {
            MacroInput macroInput;

            int downValue = 0;
            int playerValue = 0;

            if (!(
                inputStream >>
                macroInput.step >>
                downValue >>
                macroInput.button >>
                playerValue
            )) {
                return false;
            }

            if (
                downValue != 0 &&
                downValue != 1
            ) {
                return false;
            }

            if (
                playerValue != 0 &&
                playerValue != 1
            ) {
                return false;
            }

            if (
                macroInput.button < 1 ||
                macroInput.button > 3
            ) {
                return false;
            }

            macroInput.down =
                downValue == 1;

            macroInput.isPlayer1 =
                playerValue == 1;

            loadedMacro.inputs.push_back(
                macroInput
            );
        }

        std::stable_sort(
            loadedMacro.inputs.begin(),
            loadedMacro.inputs.end(),
            [](
                MacroInput const& first,
                MacroInput const& second
            ) {
                return
                    first.step <
                    second.step;
            }
        );

        if (!isPlayableMacro(loadedMacro)) {
            return false;
        }

        result = std::move(
            loadedMacro
        );

        return true;
    }

    bool exportMacro(
        GJGameLevel* level,
        std::string const& data,
        std::filesystem::path& exportedPath
    ) {
        if (!level || data.empty()) {
            return false;
        }

        auto exportDirectory =
            Mod::get()->getSaveDir() /
            "exports";

        std::error_code directoryError;

        std::filesystem::create_directories(
            exportDirectory,
            directoryError
        );

        if (directoryError) {
            log::error(
                "Replay Gallery: Could not create "
                "the export directory: {}",
                directoryError.message()
            );

            return false;
        }

        exportedPath =
            exportDirectory /
            macroFilename(level);

        std::ofstream output(
            exportedPath,
            std::ios::binary |
            std::ios::trunc
        );

        if (!output) {
            log::error(
                "Replay Gallery: Could not open the "
                "macro export file for writing"
            );

            return false;
        }

        output.write(
            data.data(),
            static_cast<std::streamsize>(
                data.size()
            )
        );

        output.close();

        if (output.fail()) {
            log::error(
                "Replay Gallery: Writing the macro "
                "export file failed"
            );

            return false;
        }

        return true;
    }

    bool isActiveGameLayer(
        GJBaseGameLayer* layer
    ) {
        if (
            !layer ||
            !g_activePlayLayer
        ) {
            return false;
        }

        return
            static_cast<GJBaseGameLayer*>(
                g_activePlayLayer
            ) == layer;
    }

    void clearActiveSession() {
        g_activePlayLayer = nullptr;

        g_recording = false;
        g_playback = false;
        g_injectingInput = false;

        g_attemptInvalid = false;
        g_attemptFinished = false;

        g_safePlaybackActive = false;

        g_currentMacro = {};
        g_playbackIndex = 0;
        g_attemptStep = 0;
    }

    void clearPendingPlayback() {
        g_pendingPlayback = false;

        g_pendingLevelID = 0;
        g_pendingLevelVersion = 0;

        g_pendingMacro = {};
    }

    std::string trimFeedbackText(
        std::string text
    ) {
        auto isWhitespace =
            [](unsigned char character) {
                return std::isspace(character) != 0;
            };

        while (
            !text.empty() &&
            isWhitespace(
                static_cast<unsigned char>(
                    text.front()
                )
            )
        ) {
            text.erase(
                text.begin()
            );
        }

        while (
            !text.empty() &&
            isWhitespace(
                static_cast<unsigned char>(
                    text.back()
                )
            )
        ) {
            text.pop_back();
        }

        return text;
    }

    std::string getFeedbackUsername() {
        std::string username =
            "Unknown";

        if (
            auto* gameManager =
                GameManager::get()
        ) {
            username =
                gameManager->m_playerName;
        }

        if (username.empty()) {
            username = "Unknown";
        }

        if (username.size() > 32) {
            username.resize(32);
        }

        return username;
    }

    std::string makeFeedbackJSON(
        std::string const& type,
        std::string const& message
    ) {
        auto createdAt =
            std::chrono::duration_cast<
                std::chrono::milliseconds
            >(
                std::chrono::system_clock::now()
                    .time_since_epoch()
            ).count();

        std::string body =
            "{";

        body +=
            "\"type\":" +
            encodeJSONString(type);

        body +=
            ",\"message\":" +
            encodeJSONString(message);

        body +=
            ",\"username\":" +
            encodeJSONString(
                getFeedbackUsername()
            );

        body +=
            fmt::format(
                ",\"levelID\":{}",
                std::max(
                    0,
                    g_feedbackLevelID
                )
            );

        body +=
            fmt::format(
                ",\"levelVersion\":{}",
                std::max(
                    0,
                    g_feedbackLevelVersion
                )
            );

        body +=
            ",\"modVersion\":" +
            encodeJSONString(
                FEEDBACK_MOD_VERSION
            );

        body +=
            fmt::format(
                ",\"createdAt\":{}",
                createdAt
            );

        body += "}";

        return body;
    }

    std::int64_t currentUnixSeconds() {
        return std::chrono::duration_cast<
            std::chrono::seconds
        >(
            std::chrono::system_clock::now()
                .time_since_epoch()
        ).count();
    }

    std::int64_t feedbackCooldownRemaining() {
        auto lastSent =
            Mod::get()->getSavedValue<
                std::int64_t
            >(
                FEEDBACK_LAST_SENT_KEY,
                0
            );

        if (lastSent <= 0) {
            return 0;
        }

        auto now =
            currentUnixSeconds();

        // If the computer clock moved backward, keep the
        // full cooldown instead of accidentally allowing
        // repeated submissions.
        if (now < lastSent) {
            return FEEDBACK_COOLDOWN_SECONDS;
        }

        auto elapsed =
            now - lastSent;

        if (
            elapsed >=
            FEEDBACK_COOLDOWN_SECONDS
        ) {
            return 0;
        }

        return
            FEEDBACK_COOLDOWN_SECONDS -
            elapsed;
    }

    std::string formatCooldownTime(
        std::int64_t remainingSeconds
    ) {
        if (remainingSeconds <= 0) {
            return "now";
        }

        auto minutes =
            (
                remainingSeconds +
                59
            ) / 60;

        if (minutes >= 60) {
            return "1 hour";
        }

        return fmt::format(
            "{} minute{}",
            minutes,
            minutes == 1
                ? ""
                : "s"
        );
    }

    constexpr std::size_t MIN_PUBLIC_RESET_VOTES = 5;

    constexpr char VOTE_DEVICE_ID_KEY[] =
        "public-vote-device-id-v1";

    struct MacroVoteCounts {
        std::size_t removeVotes = 0;
        std::size_t keepVotes = 0;
        std::string currentVote;

        std::size_t totalVotes() const {
            return removeVotes + keepVotes;
        }

        bool shouldResetPublicMacro() const {
            auto total = totalVotes();

            return
                total >= MIN_PUBLIC_RESET_VOTES &&
                removeVotes * 2 > total;
        }
    };

    std::uint64_t mixVoteIDValue(
        std::uint64_t value
    ) {
        value += 0x9e3779b97f4a7c15ULL;
        value =
            (value ^ (value >> 30)) *
            0xbf58476d1ce4e5b9ULL;
        value =
            (value ^ (value >> 27)) *
            0x94d049bb133111ebULL;

        return value ^ (value >> 31);
    }

    bool isHexIdentifier(
        std::string_view value,
        std::size_t expectedLength
    ) {
        if (value.size() != expectedLength) {
            return false;
        }

        return std::all_of(
            value.begin(),
            value.end(),
            [](unsigned char character) {
                return std::isxdigit(character) != 0;
            }
        );
    }

    std::string getOrCreateVoteDeviceID() {
        std::string existing =
            Mod::get()->getSavedValue<std::string>(
                VOTE_DEVICE_ID_KEY,
                ""
            );

        if (isHexIdentifier(existing, 32)) {
            return existing;
        }

        auto now =
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<
                    std::chrono::nanoseconds
                >(
                    std::chrono::high_resolution_clock::now()
                        .time_since_epoch()
                ).count()
            );

        std::random_device randomDevice;

        std::uint64_t first =
            now ^
            (
                static_cast<std::uint64_t>(
                    randomDevice()
                ) << 32
            ) ^
            static_cast<std::uint64_t>(
                randomDevice()
            );

        std::uint64_t second =
            (now << 1) ^
            (
                static_cast<std::uint64_t>(
                    randomDevice()
                ) << 32
            ) ^
            static_cast<std::uint64_t>(
                randomDevice()
            ) ^
            static_cast<std::uint64_t>(
                reinterpret_cast<std::uintptr_t>(
                    Mod::get()
                )
            );

        existing = fmt::format(
            "{:016x}{:016x}",
            mixVoteIDValue(first),
            mixVoteIDValue(second)
        );

        Mod::get()->setSavedValue<std::string>(
            VOTE_DEVICE_ID_KEY,
            existing
        );

        return existing;
    }

    std::string macroVoteID(
        std::string_view macroText,
        std::string_view /*firebaseETagValue*/
    ) {
        // v1.2.5 keys votes to the exact replay text only. The
        // trusted scheduled moderator calculates the same hash
        // before deleting, so votes for an old replay can never
        // remove a replacement replay.
        std::uint64_t hash =
            1469598103934665603ULL;

        for (unsigned char byte : macroText) {
            hash ^= byte;
            hash *= 1099511628211ULL;
        }

        return fmt::format(
            "{:016x}",
            hash
        );
    }

    std::string remoteCacheKey(
        int levelID
    ) {
        return fmt::format(
            "replay-gallery-firebase-cache-v1-{}",
            std::max(0, levelID)
        );
    }

    std::string legacyRemoteCacheKey(
        int levelID
    ) {
        return fmt::format(
            "showcase-firebase-cache-v2-{}",
            std::max(0, levelID)
        );
    }

    std::string remoteMacroURL(
        int levelID
    ) {
        return fmt::format(
            "{}/macros/{}.json",
            FIREBASE_DATABASE_URL,
            std::max(0, levelID)
        );
    }

    std::string macroVotesURL(
        int levelID,
        std::string_view macroID
    ) {
        return fmt::format(
            "{}/macro-votes/{}/{}.json",
            FIREBASE_DATABASE_URL,
            std::max(0, levelID),
            macroID
        );
    }

    std::string macroVoteDeviceURL(
        int levelID,
        std::string_view macroID,
        std::string_view deviceID
    ) {
        return fmt::format(
            "{}/macro-votes/{}/{}/{}.json",
            FIREBASE_DATABASE_URL,
            std::max(0, levelID),
            macroID,
            deviceID
        );
    }

    bool isJSONNull(
        std::string_view text
    ) {
        auto first =
            text.find_first_not_of(
                " \t\r\n"
            );

        auto last =
            text.find_last_not_of(
                " \t\r\n"
            );

        return
            first != std::string_view::npos &&
            last != std::string_view::npos &&
            text.substr(
                first,
                last - first + 1
            ) == "null";
    }

    void skipJSONWhitespace(
        std::string_view text,
        std::size_t& position
    ) {
        while (
            position < text.size() &&
            std::isspace(
                static_cast<unsigned char>(
                    text[position]
                )
            ) != 0
        ) {
            ++position;
        }
    }

    bool parseJSONStringToken(
        std::string_view text,
        std::size_t& position,
        std::string& result
    ) {
        skipJSONWhitespace(
            text,
            position
        );

        if (
            position >= text.size() ||
            text[position] != '"'
        ) {
            return false;
        }

        std::size_t start = position;
        bool escaped = false;

        ++position;

        while (position < text.size()) {
            char character = text[position++];

            if (escaped) {
                escaped = false;
                continue;
            }

            if (character == '\\') {
                escaped = true;
                continue;
            }

            if (character == '"') {
                return decodeJSONString(
                    std::string(
                        text.substr(
                            start,
                            position - start
                        )
                    ),
                    result
                );
            }
        }

        return false;
    }

    bool parseMacroVotes(
        std::string_view json,
        std::string_view currentDeviceID,
        MacroVoteCounts& result
    ) {
        result = {};

        if (isJSONNull(json)) {
            return true;
        }

        std::size_t position = 0;
        std::size_t entries = 0;

        skipJSONWhitespace(
            json,
            position
        );

        if (
            position >= json.size() ||
            json[position] != '{'
        ) {
            return false;
        }

        ++position;

        while (true) {
            skipJSONWhitespace(
                json,
                position
            );

            if (position >= json.size()) {
                return false;
            }

            if (json[position] == '}') {
                ++position;
                break;
            }

            if (++entries > 10000) {
                return false;
            }

            std::string deviceID;
            std::string vote;

            if (!parseJSONStringToken(
                json,
                position,
                deviceID
            )) {
                return false;
            }

            skipJSONWhitespace(
                json,
                position
            );

            if (
                position >= json.size() ||
                json[position] != ':'
            ) {
                return false;
            }

            ++position;

            if (!parseJSONStringToken(
                json,
                position,
                vote
            )) {
                return false;
            }

            if (vote == "remove") {
                ++result.removeVotes;
            }
            else if (vote == "keep") {
                ++result.keepVotes;
            }

            if (deviceID == currentDeviceID) {
                result.currentVote = vote;
            }

            skipJSONWhitespace(
                json,
                position
            );

            if (position >= json.size()) {
                return false;
            }

            if (json[position] == ',') {
                ++position;
                continue;
            }

            if (json[position] == '}') {
                ++position;
                break;
            }

            return false;
        }

        skipJSONWhitespace(
            json,
            position
        );

        return position == json.size();
    }

    std::string firebaseETag(
        web::WebResponse& response
    ) {
        for (auto const headerName : {
            "ETag",
            "Etag",
            "etag"
        }) {
            if (auto value = response.header(headerName)) {
                std::string result(
                    value->data(),
                    value->size()
                );

                if (!result.empty()) {
                    return result;
                }
            }
        }

        return "";
    }

    void clearVotesForVoteID(
        int levelID,
        std::string_view voteID
    ) {
        // Vote records are keyed to the exact replay contents and ETag.
        // A replacement replay receives a different vote ID, so old votes
        // cannot affect it. Do not request parent-node deletion here: the
        // safe public Firebase rules intentionally allow each installation
        // to write only its own child vote, not erase everyone else's votes.
        if (levelID > 0 && !voteID.empty()) {
            log::debug(
                "Replay Gallery: Leaving isolated old vote set {} for level {}",
                voteID,
                levelID
            );
        }
    }

}

// ---------------------------------------------------------
// Community voting for the current public replay
// ---------------------------------------------------------

class ReplayGalleryVotePopup :
    public Popup {
protected:
    int m_levelID = 0;

    std::string m_macroText;
    std::string m_macroID;
    std::string m_expectedETag;
    std::string m_deviceID;

    MacroData m_macro;
    MacroVoteCounts m_counts;

    CCLabelBMFont* m_resultLabel = nullptr;
    CCLabelBMFont* m_countsLabel = nullptr;
    CCLabelBMFont* m_ruleLabel = nullptr;
    CCLabelBMFont* m_statusLabel = nullptr;

    ButtonSprite* m_keepSprite = nullptr;
    ButtonSprite* m_removeSprite = nullptr;

    async::TaskHolder<web::WebResponse>
        m_voteWriteRequest;

    async::TaskHolder<web::WebResponse>
        m_votesReadRequest;

    async::TaskHolder<web::WebResponse>
        m_macroReadRequest;

    async::TaskHolder<web::WebResponse>
        m_macroResetRequest;

    bool m_resetting = false;

    bool init(
        int levelID,
        std::string const& macroText,
        std::string const& expectedETag
    ) {
        if (
            levelID <= 0 ||
            macroText.empty() ||
            expectedETag.empty() ||
            !deserializeMacro(
                macroText,
                m_macro
            ) ||
            !isPlayableMacro(m_macro)
        ) {
            return false;
        }

        if (!Popup::init(
            380.f,
            245.f
        )) {
            return false;
        }

        m_levelID = levelID;
        m_macroText = macroText;
        m_expectedETag = expectedETag;
        m_macroID = macroVoteID(
            macroText,
            expectedETag
        );
        m_deviceID = getOrCreateVoteDeviceID();

        this->setTitle(
            "Public Replay Vote"
        );

        auto* explanation =
            CCLabelBMFont::create(
                "Vote Remove only if this public replay is noclipped, bugged, or unusable.",
                "bigFont.fnt"
            );

        explanation->setScale(
            0.32f
        );

        explanation->setPosition({
            190.f,
            198.f
        });

        explanation->limitLabelWidth(
            330.f,
            0.32f,
            0.24f
        );

        m_mainLayer->addChild(
            explanation
        );

        m_resultLabel =
            CCLabelBMFont::create(
                fmt::format(
                    "Current public run: {}",
                    formatRunResult(m_macro)
                ).c_str(),
                "bigFont.fnt"
            );

        m_resultLabel->setScale(
            0.42f
        );

        m_resultLabel->setPosition({
            190.f,
            170.f
        });

        m_mainLayer->addChild(
            m_resultLabel
        );

        m_countsLabel =
            CCLabelBMFont::create(
                "Remove: 0   Keep: 0",
                "bigFont.fnt"
            );

        m_countsLabel->setScale(
            0.42f
        );

        m_countsLabel->setPosition({
            190.f,
            140.f
        });

        m_mainLayer->addChild(
            m_countsLabel
        );

        m_ruleLabel =
            CCLabelBMFont::create(
                "Removal requires 5 votes and more than 50% Remove; moderation runs every few minutes.",
                "chatFont.fnt"
            );

        m_ruleLabel->setScale(
            0.55f
        );

        m_ruleLabel->setPosition({
            190.f,
            116.f
        });

        m_ruleLabel->limitLabelWidth(
            335.f,
            0.55f,
            0.42f
        );

        m_mainLayer->addChild(
            m_ruleLabel
        );

        m_keepSprite =
            ButtonSprite::create(
                "Keep",
                0.70f
            );

        auto* keepButton =
            CCMenuItemSpriteExtra::create(
                m_keepSprite,
                this,
                menu_selector(
                    ReplayGalleryVotePopup::
                        onKeep
                )
            );

        keepButton->setPosition({
            115.f,
            77.f
        });

        keepButton->setID(
            "nonothenonokid.replay-gallery/"
            "vote-keep-button"
        );

        m_buttonMenu->addChild(
            keepButton
        );

        m_removeSprite =
            ButtonSprite::create(
                "Remove",
                0.70f
            );

        auto* removeButton =
            CCMenuItemSpriteExtra::create(
                m_removeSprite,
                this,
                menu_selector(
                    ReplayGalleryVotePopup::
                        onRemove
                )
            );

        removeButton->setPosition({
            265.f,
            77.f
        });

        removeButton->setID(
            "nonothenonokid.replay-gallery/"
            "vote-remove-button"
        );

        m_buttonMenu->addChild(
            removeButton
        );

        m_statusLabel =
            CCLabelBMFont::create(
                "Loading votes...",
                "chatFont.fnt"
            );

        m_statusLabel->setScale(
            0.58f
        );

        m_statusLabel->setPosition({
            190.f,
            38.f
        });

        m_statusLabel->limitLabelWidth(
            335.f,
            0.58f,
            0.40f
        );

        m_mainLayer->addChild(
            m_statusLabel
        );

        updateVoteUI();
        refreshVotes(true);

        return true;
    }

    void onKeep(CCObject*) {
        submitVote(
            "keep"
        );
    }

    void onRemove(CCObject*) {
        submitVote(
            "remove"
        );
    }

    void submitVote(
        std::string const& vote
    ) {
        if (
            m_resetting ||
            m_voteWriteRequest.isPending() ||
            m_votesReadRequest.isPending() ||
            m_macroReadRequest.isPending() ||
            m_macroResetRequest.isPending()
        ) {
            setStatus(
                "Please wait for the current request."
            );

            return;
        }

        auto request = web::WebRequest();

        request.userAgent(
            "Replay-Gallery/1.2.5"
        );

        request.header(
            "Content-Type",
            "application/json"
        );

        request.timeout(
            std::chrono::seconds(10)
        );

        request.bodyString(
            encodeJSONString(vote)
        );

        setStatus(
            vote == "remove"
                ? "Submitting Remove vote..."
                : "Submitting Keep vote..."
        );

        m_voteWriteRequest.spawn(
            "Submitting Replay Gallery vote",
            request.put(
                macroVoteDeviceURL(
                    m_levelID,
                    m_macroID,
                    m_deviceID
                )
            ),
            [this, vote](
                web::WebResponse response
            ) {
                if (!response.ok()) {
                    if (
                        response.code() == 401 ||
                        response.code() == 403
                    ) {
                        setStatus(
                            "Voting is blocked by Firebase rules (HTTP 401)."
                        );
                    }
                    else {
                        setStatus(
                            fmt::format(
                                "Vote failed (HTTP {}).",
                                response.code()
                            )
                        );
                    }

                    log::warn(
                        "Replay Gallery: Public replay vote failed "
                        "for level {} with HTTP {}: {}",
                        m_levelID,
                        response.code(),
                        response.errorMessage()
                    );

                    return;
                }

                m_counts.currentVote = vote;
                updateVoteUI();
                refreshVotes(true);
            }
        );
    }

    void refreshVotes(
        bool allowReset
    ) {
        if (m_votesReadRequest.isPending()) {
            return;
        }

        auto request = web::WebRequest();

        request.userAgent(
            "Replay-Gallery/1.2.5"
        );

        request.header(
            "Cache-Control",
            "no-cache"
        );

        request.header(
            "Pragma",
            "no-cache"
        );

        request.timeout(
            std::chrono::seconds(8)
        );

        m_votesReadRequest.spawn(
            "Loading Replay Gallery votes",
            request.get(
                macroVotesURL(
                    m_levelID,
                    m_macroID
                )
            ),
            [this, allowReset](
                web::WebResponse response
            ) {
                if (!response.ok()) {
                    if (
                        response.code() == 401 ||
                        response.code() == 403
                    ) {
                        setStatus(
                            "Voting is blocked by Firebase rules (HTTP 401)."
                        );
                    }
                    else {
                        setStatus(
                            fmt::format(
                                "Could not load votes (HTTP {}).",
                                response.code()
                            )
                        );
                    }

                    return;
                }

                MacroVoteCounts counts;

                std::string body =
                    response
                        .string()
                        .unwrapOr("");

                if (!parseMacroVotes(
                    body,
                    m_deviceID,
                    counts
                )) {
                    setStatus(
                        "Firebase returned invalid vote data."
                    );

                    log::warn(
                        "Replay Gallery: Invalid vote data for "
                        "level {} and macro {}",
                        m_levelID,
                        m_macroID
                    );

                    return;
                }

                m_counts = std::move(counts);
                updateVoteUI();

                if (
                    allowReset &&
                    m_counts.shouldResetPublicMacro()
                ) {
                    waitForServerReset();
                    return;
                }

                if (m_counts.currentVote == "remove") {
                    setStatus(
                        "Your current vote: Remove"
                    );
                }
                else if (m_counts.currentVote == "keep") {
                    setStatus(
                        "Your current vote: Keep"
                    );
                }
                else {
                    setStatus(
                        "You have not voted on this run yet."
                    );
                }
            }
        );
    }

    void waitForServerReset() {
        if (m_resetting) {
            return;
        }

        m_resetting = true;

        setStatus(
            "Majority reached. Waiting for the moderation check..."
        );

        // Deletion is intentionally performed by a trusted scheduled
        // moderator, not by an untrusted game client. Check once for
        // confirmation; reopening the popup checks again later.
        auto* delay =
            CCDelayTime::create(1.5f);

        auto* callback =
            CCCallFunc::create(
                this,
                callfunc_selector(
                    ReplayGalleryVotePopup::checkServerReset
                )
            );

        this->runAction(
            CCSequence::create(
                delay,
                callback,
                nullptr
            )
        );
    }

    void checkServerReset() {
        if (m_macroReadRequest.isPending()) {
            m_resetting = false;
            return;
        }

        auto request = web::WebRequest();

        request.userAgent(
            "Replay-Gallery/1.2.5"
        );

        request.header(
            "Cache-Control",
            "no-cache"
        );

        request.header(
            "Pragma",
            "no-cache"
        );

        request.timeout(
            std::chrono::seconds(8)
        );

        m_macroReadRequest.spawn(
            "Checking Replay Gallery server moderation",
            request.get(
                remoteMacroURL(m_levelID)
            ),
            [this](
                web::WebResponse response
            ) {
                m_resetting = false;

                if (!response.ok()) {
                    setStatus(
                        fmt::format(
                            "Could not confirm removal (HTTP {}).",
                            response.code()
                        )
                    );
                    return;
                }

                std::string body =
                    response
                        .string()
                        .unwrapOr("");

                if (isJSONNull(body)) {
                    clearLocalPublicCache();

                    setStatus(
                        "Public replay removed by community vote."
                    );

                    log::info(
                        "Replay Gallery: Server removed the public "
                        "macro for level {} after {} Remove and {} Keep votes",
                        m_levelID,
                        m_counts.removeVotes,
                        m_counts.keepVotes
                    );
                    return;
                }

                std::string currentMacroText;

                if (
                    decodeJSONString(
                        body,
                        currentMacroText
                    ) &&
                    currentMacroText != m_macroText
                ) {
                    clearLocalPublicCache();

                    setStatus(
                        "The public replay changed; these votes no longer apply."
                    );
                    return;
                }

                setStatus(
                    "Majority reached. Removal may take up to about 5 minutes."
                );
            }
        );
    }

    void clearLocalPublicCache() {
        Mod::get()->setSavedValue<std::string>(
            remoteCacheKey(m_levelID),
            ""
        );

        Mod::get()->setSavedValue<std::string>(
            legacyRemoteCacheKey(m_levelID),
            ""
        );
    }

    void updateVoteUI() {
        if (m_countsLabel) {
            m_countsLabel->setString(
                fmt::format(
                    "Remove: {}   Keep: {}",
                    m_counts.removeVotes,
                    m_counts.keepVotes
                ).c_str()
            );
        }

        if (m_removeSprite) {
            m_removeSprite->setOpacity(
                m_counts.currentVote == "remove"
                    ? 255
                    : 150
            );
        }

        if (m_keepSprite) {
            m_keepSprite->setOpacity(
                m_counts.currentVote == "keep"
                    ? 255
                    : 150
            );
        }
    }

    void setStatus(
        std::string const& text
    ) {
        if (!m_statusLabel) {
            return;
        }

        m_statusLabel->setString(
            text.c_str()
        );

        m_statusLabel->limitLabelWidth(
            335.f,
            0.58f,
            0.40f
        );
    }

public:
    static ReplayGalleryVotePopup* create(
        int levelID,
        std::string const& macroText,
        std::string const& expectedETag
    ) {
        auto* result =
            new ReplayGalleryVotePopup();

        if (result->init(
            levelID,
            macroText,
            expectedETag
        )) {
            result->autorelease();
            return result;
        }

        delete result;
        return nullptr;
    }
};

// ---------------------------------------------------------
// Suggestion and bug-report popup
// ---------------------------------------------------------

class ReplayGalleryFeedbackPopup :
    public Popup {
protected:
    TextInput* m_messageInput = nullptr;

    ButtonSprite* m_bugSprite = nullptr;
    ButtonSprite* m_suggestionSprite = nullptr;

    CCLabelBMFont* m_counterLabel = nullptr;
    CCLabelBMFont* m_selectedTypeLabel = nullptr;
    CCLabelBMFont* m_statusLabel = nullptr;

    bool m_isBug = false;

    bool init() {
        if (!Popup::init(
            380.f,
            240.f
        )) {
            return false;
        }

        this->setTitle(
            "Replay Gallery Feedback"
        );

        auto* instruction =
            CCLabelBMFont::create(
                "Choose a type, then enter your message.",
                "bigFont.fnt"
            );

        instruction->setScale(
            0.38f
        );

        instruction->setPosition({
            190.f,
            193.f
        });

        m_mainLayer->addChild(
            instruction
        );

        m_bugSprite =
            ButtonSprite::create(
                "Bug Report",
                0.62f
            );

        auto* bugButton =
            CCMenuItemSpriteExtra::create(
                m_bugSprite,
                this,
                menu_selector(
                    ReplayGalleryFeedbackPopup::
                        onBug
                )
            );

        bugButton->setPosition({
            115.f,
            164.f
        });

        bugButton->setID(
            "nonothenonokid.replay-gallery/"
            "feedback-bug-button"
        );

        m_buttonMenu->addChild(
            bugButton
        );

        m_suggestionSprite =
            ButtonSprite::create(
                "Suggestion",
                0.62f
            );

        auto* suggestionButton =
            CCMenuItemSpriteExtra::create(
                m_suggestionSprite,
                this,
                menu_selector(
                    ReplayGalleryFeedbackPopup::
                        onSuggestion
                )
            );

        suggestionButton->setPosition({
            265.f,
            164.f
        });

        suggestionButton->setID(
            "nonothenonokid.replay-gallery/"
            "feedback-suggestion-button"
        );

        m_buttonMenu->addChild(
            suggestionButton
        );

        m_selectedTypeLabel =
            CCLabelBMFont::create(
                "Selected: Suggestion",
                "bigFont.fnt"
            );

        m_selectedTypeLabel->setScale(
            0.34f
        );

        m_selectedTypeLabel->setPosition({
            190.f,
            142.f
        });

        m_selectedTypeLabel->setID(
            "nonothenonokid.replay-gallery/"
            "feedback-selected-type"
        );

        m_mainLayer->addChild(
            m_selectedTypeLabel
        );

        m_messageInput =
            TextInput::create(
                320.f,
                "Describe the bug or suggestion...",
                "bigFont.fnt"
            );

        m_messageInput->setMaxCharCount(
            500
        );

        m_messageInput->setPosition({
            190.f,
            111.f
        });

        m_messageInput->setID(
            "nonothenonokid.replay-gallery/"
            "feedback-message-input"
        );

        m_messageInput->setCallback(
            [this](
                std::string const&
            ) {
                updateCounter();
            }
        );

        m_mainLayer->addChild(
            m_messageInput
        );

        m_counterLabel =
            CCLabelBMFont::create(
                "0 / 500",
                "chatFont.fnt"
            );

        m_counterLabel->setScale(
            0.55f
        );

        m_counterLabel->setAnchorPoint({
            1.f,
            0.5f
        });

        m_counterLabel->setPosition({
            350.f,
            84.f
        });

        m_mainLayer->addChild(
            m_counterLabel
        );

        m_statusLabel =
            CCLabelBMFont::create(
                "",
                "chatFont.fnt"
            );

        m_statusLabel->setScale(
            0.55f
        );

        m_statusLabel->setPosition({
            190.f,
            67.f
        });

        m_mainLayer->addChild(
            m_statusLabel
        );

        auto* submitSprite =
            ButtonSprite::create(
                "Submit",
                0.78f
            );

        auto* submitButton =
            CCMenuItemSpriteExtra::create(
                submitSprite,
                this,
                menu_selector(
                    ReplayGalleryFeedbackPopup::
                        onSubmit
                )
            );

        submitButton->setPosition({
            190.f,
            35.f
        });

        submitButton->setID(
            "nonothenonokid.replay-gallery/"
            "feedback-submit-button"
        );

        m_buttonMenu->addChild(
            submitButton
        );

        updateTypeButtons();
        updateCounter();

        auto remaining =
            feedbackCooldownRemaining();

        if (remaining > 0) {
            setStatus(
                fmt::format(
                    "You can send again in {}.",
                    formatCooldownTime(
                        remaining
                    )
                )
            );
        }

        return true;
    }

    void onBug(CCObject*) {
        m_isBug = true;

        updateTypeButtons();
    }

    void onSuggestion(CCObject*) {
        m_isBug = false;

        updateTypeButtons();
    }

    void onSubmit(CCObject*) {
        if (g_feedbackSending) {
            setStatus(
                "A report is already being sent."
            );

            return;
        }

        auto remaining =
            feedbackCooldownRemaining();

        if (remaining > 0) {
            setStatus(
                fmt::format(
                    "Wait {} before sending again.",
                    formatCooldownTime(
                        remaining
                    )
                )
            );

            return;
        }

        std::string message =
            trimFeedbackText(
                m_messageInput
                    ? std::string(
                        m_messageInput
                            ->getString()
                    )
                    : std::string()
            );

        if (message.size() < 3) {
            setStatus(
                "Enter at least 3 characters."
            );

            return;
        }

        if (message.size() > 500) {
            message.resize(500);
        }

        std::string type =
            m_isBug
                ? "bug"
                : "suggestion";

        std::string body =
            makeFeedbackJSON(
                type,
                message
            );

        std::string url =
            fmt::format(
                "{}/reports.json",
                FIREBASE_DATABASE_URL
            );

        auto request =
            web::WebRequest();

        request.userAgent(
            "Replay-Gallery/1.2.5"
        );

        request.header(
            "Content-Type",
            "application/json"
        );

        request.timeout(
            std::chrono::seconds(15)
        );

        request.bodyString(
            body
        );

        g_feedbackSending = true;

        async::spawn(
            request.post(url),
            [](
                web::WebResponse response
            ) {
                g_feedbackSending = false;

                if (response.ok()) {
                    Mod::get()->setSavedValue<
                        std::int64_t
                    >(
                        FEEDBACK_LAST_SENT_KEY,
                        currentUnixSeconds()
                    );

                    FLAlertLayer::create(
                        "Feedback Sent",
                        "Thanks! Your report was sent.\n\n"
                        "You can send another report in "
                        "one hour.",
                        "OK"
                    )->show();

                    log::info(
                        "Replay Gallery: Feedback "
                        "submission succeeded; the "
                        "one-hour cooldown started"
                    );

                    return;
                }

                std::string errorText =
                    fmt::format(
                        "The report could not be sent.\\n\\n"
                        "Firebase returned HTTP {}.",
                        response.code()
                    );

                FLAlertLayer::create(
                    "Feedback Failed",
                    errorText.c_str(),
                    "OK"
                )->show();

                log::warn(
                    "Replay Gallery: Feedback upload "
                    "failed with HTTP {}: {}",
                    response.code(),
                    response.errorMessage()
                );
            }
        );

        this->onClose(
            nullptr
        );
    }

    void updateTypeButtons() {
        if (m_bugSprite) {
            m_bugSprite->setOpacity(
                m_isBug
                    ? 255
                    : 120
            );
        }

        if (m_suggestionSprite) {
            m_suggestionSprite->setOpacity(
                m_isBug
                    ? 120
                    : 255
            );
        }

        if (m_selectedTypeLabel) {
            m_selectedTypeLabel->setString(
                m_isBug
                    ? "Selected: Bug Report"
                    : "Selected: Suggestion"
            );
        }
    }

    void updateCounter() {
        if (
            !m_counterLabel ||
            !m_messageInput
        ) {
            return;
        }

        std::string text =
            m_messageInput->getString();

        m_counterLabel->setString(
            fmt::format(
                "{} / 500",
                text.size()
            ).c_str()
        );
    }

    void setStatus(
        std::string const& text
    ) {
        if (m_statusLabel) {
            m_statusLabel->setString(
                text.c_str()
            );
        }
    }

public:
    static ReplayGalleryFeedbackPopup* create() {
        auto* result =
            new ReplayGalleryFeedbackPopup();

        if (result->init()) {
            result->autorelease();

            return result;
        }

        delete result;

        return nullptr;
    }
};

// ---------------------------------------------------------
// Feedback button on the Geometry Dash main menu
// ---------------------------------------------------------

class $modify(
    ReplayGalleryMenuLayer,
    MenuLayer
) {
public:
    bool init() {
        if (!MenuLayer::init()) {
            return false;
        }

        // Remove only a duplicate created by this mod. Never delete
        // another active mod's UI because it may still own pointers
        // to those nodes.
        removeChildrenByID(
            this,
            "nonothenonokid.replay-gallery/feedback-menu"
        );


        auto windowSize =
            CCDirector::sharedDirector()
                ->getWinSize();

        auto* feedbackMenu =
            CCMenu::create();

        feedbackMenu->setPosition({
            0.f,
            0.f
        });

        feedbackMenu->setID(
            "nonothenonokid.replay-gallery/"
            "feedback-menu"
        );

        auto* feedbackSprite =
            ButtonSprite::create(
                "Replay Gallery Feedback",
                0.48f
            );

        auto* feedbackButton =
            CCMenuItemSpriteExtra::create(
                feedbackSprite,
                this,
                menu_selector(
                    ReplayGalleryMenuLayer::
                        onFeedback
                )
            );

        feedbackButton->setPosition({
            windowSize.width - 105.f,
            windowSize.height - 25.f
        });

        feedbackButton->setID(
            "nonothenonokid.replay-gallery/"
            "feedback-button"
        );

        feedbackMenu->addChild(
            feedbackButton
        );

        this->addChild(
            feedbackMenu,
            50
        );

        return true;
    }

    void onFeedback(CCObject*) {
        auto* popup =
            ReplayGalleryFeedbackPopup::create();

        if (popup) {
            popup->show();
        }
    }
};

// ---------------------------------------------------------
// Built-in Replay Gallery Safe Mode
// ---------------------------------------------------------
//
// Geometry Dash normally records a completed level through
// GJGameLevel::savePercentage and may also save score data.
// During public macro playback, block both save paths.
//
// The PlayLayer code below also sets m_isTestMode and the
// level's m_dontSave flag. These hooks are an extra backstop
// so a public macro can never become a real completion.

class $modify(
    ReplayGallerySafeGameLevel,
    GJGameLevel
) {
public:
    void savePercentage(
        int percent,
        bool isPracticeMode,
        int clicks,
        int attempts,
        bool isChkValid
    ) {
        bool blockSave =
            g_safePlaybackActive &&
            g_activePlayLayer &&
            g_activePlayLayer->m_level == this;

        if (blockSave) {
            log::info(
                "Replay Gallery: Safe Mode blocked "
                "savePercentage({}%)",
                percent
            );

            return;
        }

        GJGameLevel::savePercentage(
            percent,
            isPracticeMode,
            clicks,
            attempts,
            isChkValid
        );
    }

    void saveNewScore(
        int value,
        int type,
        int ticks,
        int clicks,
        int coins,
        gd::string inputs,
        bool save
    ) {
        bool blockSave =
            g_safePlaybackActive &&
            g_activePlayLayer &&
            g_activePlayLayer->m_level == this;

        if (blockSave) {
            log::info(
                "Replay Gallery: Safe Mode blocked "
                "saveNewScore"
            );

            return;
        }

        GJGameLevel::saveNewScore(
            value,
            type,
            ticks,
            clicks,
            coins,
            inputs,
            save
        );
    }
};

// ---------------------------------------------------------
// Record real inputs and inject macro inputs
// ---------------------------------------------------------

class $modify(
    ReplayGalleryBaseGameLayer,
    GJBaseGameLayer
) {
public:
    void handleButton(
        bool down,
        int button,
        bool isPlayer1
    ) {
        // Do not let the viewer's clicks interfere with a
        // downloaded Replay Gallery macro. Injected macro inputs
        // set g_injectingInput while they are being applied.
        if (
            g_playback &&
            !g_injectingInput &&
            isActiveGameLayer(this)
        ) {
            return;
        }

        bool shouldRecord =
            g_recording &&
            !g_playback &&
            !g_injectingInput &&
            !g_attemptInvalid &&
            !g_attemptFinished &&
            isActiveGameLayer(this);

        GJBaseGameLayer::handleButton(
            down,
            button,
            isPlayer1
        );

        if (!shouldRecord) {
            return;
        }

        auto* playLayer =
            g_activePlayLayer;

        if (
            !playLayer ||
            !playLayer->m_started ||
            playLayer->m_playerDied ||
            playLayer
                ->m_levelEndAnimationStarted
        ) {
            return;
        }

        MacroInput recordedInput;

        recordedInput.step =
            relativeSimulationStep(this);

        recordedInput.down = down;
        recordedInput.button = button;

        recordedInput.isPlayer1 =
            isPlayer1;

        if (
            !g_currentMacro.inputs.empty()
        ) {
            MacroInput const& previous =
                g_currentMacro.inputs.back();

            bool exactDuplicate =
                previous.step ==
                    recordedInput.step &&
                previous.down ==
                    recordedInput.down &&
                previous.button ==
                    recordedInput.button &&
                previous.isPlayer1 ==
                    recordedInput.isPlayer1;

            if (exactDuplicate) {
                return;
            }
        }

        g_currentMacro.inputs.push_back(
            recordedInput
        );
    }

    void processCommands(
        float dt,
        bool isHalfTick,
        bool isLastTick
    ) {
        auto* playLayer =
            g_activePlayLayer;

        bool activeAttemptStep =
            playLayer &&
            playLayer->m_started &&
            !g_attemptFinished &&
            isActiveGameLayer(this);

        bool shouldPlay =
            g_playback &&
            activeAttemptStep;

        if (shouldPlay) {
            unsigned int position =
                macroPlaybackPosition(
                    this,
                    g_currentMacro
                );

            while (
                g_playbackIndex <
                g_currentMacro.inputs.size()
            ) {
                MacroInput const& input =
                    g_currentMacro.inputs[
                        g_playbackIndex
                    ];

                if (
                    input.step >
                    position
                ) {
                    break;
                }

                g_injectingInput = true;

                GJBaseGameLayer::handleButton(
                    input.down,
                    input.button,
                    input.isPlayer1
                );

                g_injectingInput = false;

                ++g_playbackIndex;
            }
        }

        GJBaseGameLayer::processCommands(
            dt,
            isHalfTick,
            isLastTick
        );

        if (activeAttemptStep) {
            ++g_attemptStep;
        }
    }
};

// ---------------------------------------------------------
// Automatic Firebase Replay Gallery button
// ---------------------------------------------------------

class $modify(
    ReplayGalleryLevelInfoLayer,
    LevelInfoLayer
) {
public:
    struct Fields {
        GJGameLevel* m_level = nullptr;

        CCMenuItemSpriteExtra*
            m_replayGalleryButton = nullptr;

        CCMenuItemSpriteExtra*
            m_voteButton = nullptr;

        std::string m_localMacro;
        std::string m_publicMacro;
        std::string m_publicMacroETag;
        std::string m_availableMacro;

        bool m_publicMacroCanonical = false;

        async::TaskHolder<
            web::WebResponse
        > m_remoteRequest;

        int m_publicCheckAttempts = 0;
        bool m_publicMacroFound = false;
    };

    bool init(
        GJGameLevel* level,
        bool challenge
    ) {
        if (!LevelInfoLayer::init(
            level,
            challenge
        )) {
            return false;
        }

        m_fields->m_level = level;

        g_feedbackLevelID =
            getLevelID(level);

        g_feedbackLevelVersion =
            getLevelVersion(level);

        auto* playMenu =
            this->getChildByID(
                "play-menu"
            );

        if (!playMenu) {
            log::error(
                "Replay Gallery: "
                "Could not find play-menu"
            );

            return true;
        }

        removeChildrenByID(
            playMenu,
            "nonothenonokid.replay-gallery/"
            "replay-gallery-button"
        );

        removeChildrenByID(
            playMenu,
            "nonothenonokid.replay-gallery/"
            "public-vote-button"
        );

        auto* icon =
            CCSprite::createWithSpriteFrameName(
                "GJ_searchBtn_001.png"
            );

        if (!icon) {
            log::error(
                "Replay Gallery: "
                "Could not create button icon"
            );

            return true;
        }

        icon->setScale(
            0.38f
        );

        auto* replayButton =
            CCMenuItemSpriteExtra::create(
                icon,
                this,
                menu_selector(
                    ReplayGalleryLevelInfoLayer::
                        onReplayGalleryButton
                )
            );

        replayButton->setPosition({
            40.f,
            20.f
        });

        replayButton->setID(
            "nonothenonokid.replay-gallery/"
            "replay-gallery-button"
        );

        replayButton->setVisible(false);
        replayButton->setZOrder(200);

        playMenu->addChild(
            replayButton
        );

        m_fields->m_replayGalleryButton =
            replayButton;

        auto* voteSprite =
            ButtonSprite::create(
                "Vote",
                0.45f
            );

        voteSprite->setScale(
            0.42f
        );

        auto* voteButton =
            CCMenuItemSpriteExtra::create(
                voteSprite,
                this,
                menu_selector(
                    ReplayGalleryLevelInfoLayer::
                        onPublicVoteButton
                )
            );

        // Put the smaller Vote button directly beside the Replay Gallery
        // button instead of overlapping its corner. Keep the same vertical
        // center so the two buttons are easy to see as a pair.
        voteButton->setPosition({
            78.f,
            20.f
        });

        voteButton->setZOrder(
            220
        );

        voteButton->setID(
            "nonothenonokid.replay-gallery/"
            "public-vote-button"
        );

        voteButton->setVisible(false);

        playMenu->addChild(
            voteButton
        );

        m_fields->m_voteButton =
            voteButton;

        loadSavedAndCachedMacro();

        pollForPublicMacro(0.f);

        this->schedule(
            schedule_selector(
                ReplayGalleryLevelInfoLayer::
                    pollForPublicMacro
            ),
            1.f
        );

        return true;
    }

    void onReplayGalleryButton(
        CCObject*
    ) {
        auto* level =
            m_fields->m_level;

        if (!level) {
            FLAlertLayer::create(
                "Replay Gallery",
                "Could not find this level.",
                "OK"
            )->show();

            return;
        }

        MacroData loadedMacro;

        if (
            m_fields->m_availableMacro.empty() ||
            !deserializeMacro(
                m_fields->m_availableMacro,
                loadedMacro
            )
        ) {
            FLAlertLayer::create(
                "Replay Gallery",
                "The available macro could not "
                "be read.",
                "OK"
            )->show();

            return;
        }

        if (!isPlayableMacro(loadedMacro)) {
            FLAlertLayer::create(
                "Replay Gallery",
                "The available run is missing its "
                "timing or result data. Record it again "
                "with the newest build.",
                "OK"
            )->show();

            return;
        }

        if (m_fields->m_replayGalleryButton) {
            m_fields->m_replayGalleryButton
                ->setEnabled(false);
        }

        clearPendingPlayback();

        g_pendingPlayback = true;

        g_pendingLevelID =
            getLevelID(level);

        g_pendingLevelVersion =
            getLevelVersion(level);

        g_pendingMacro =
            std::move(loadedMacro);

        log::info(
            "Replay Gallery: Starting best available run {} "
            "with {} inputs for level {} version {}",
            formatRunResult(g_pendingMacro),
            g_pendingMacro.inputs.size(),
            g_pendingLevelID,
            g_pendingLevelVersion
        );

        LevelInfoLayer::onPlay(
            m_fields->m_replayGalleryButton
        );
    }

    void onPublicVoteButton(
        CCObject*
    ) {
        auto* level =
            m_fields->m_level;

        MacroData validationMacro;

        if (
            !level ||
            !m_fields->m_publicMacroCanonical ||
            m_fields->m_publicMacro.empty() ||
            m_fields->m_publicMacroETag.empty() ||
            !deserializeMacro(
                m_fields->m_publicMacro,
                validationMacro
            ) ||
            !isPlayableMacro(validationMacro)
        ) {
            FLAlertLayer::create(
                "Public Replay Vote",
                "There is no current Firebase public replay "
                "available to vote on.",
                "OK"
            )->show();

            return;
        }

        auto* popup =
            ReplayGalleryVotePopup::create(
                getLevelID(level),
                m_fields->m_publicMacro,
                m_fields->m_publicMacroETag
            );

        if (popup) {
            popup->show();
        }
    }

private:
    void rebuildAvailableMacro() {
        m_fields->m_availableMacro.clear();

        auto considerMacro =
            [this](
                std::string const& text
            ) {
                MacroData candidate;

                if (
                    text.empty() ||
                    !deserializeMacro(
                        text,
                        candidate
                    ) ||
                    !isPlayableMacro(candidate)
                ) {
                    return;
                }

                if (
                    m_fields->m_availableMacro.empty()
                ) {
                    m_fields->m_availableMacro =
                        text;
                    return;
                }

                MacroData current;

                if (
                    !deserializeMacro(
                        m_fields->m_availableMacro,
                        current
                    ) ||
                    isBetterMacro(
                        candidate,
                        current
                    )
                ) {
                    m_fields->m_availableMacro =
                        text;
                }
            };

        considerMacro(
            m_fields->m_localMacro
        );

        considerMacro(
            m_fields->m_publicMacro
        );

        if (m_fields->m_replayGalleryButton) {
            m_fields->m_replayGalleryButton
                ->setVisible(
                    !m_fields
                        ->m_availableMacro
                        .empty()
                );
        }

        if (m_fields->m_voteButton) {
            MacroData publicValidation;

            bool canVote =
                m_fields->m_publicMacroCanonical &&
                !m_fields->m_publicMacro.empty() &&
                !m_fields->m_publicMacroETag.empty() &&
                deserializeMacro(
                    m_fields->m_publicMacro,
                    publicValidation
                ) &&
                isPlayableMacro(
                    publicValidation
                );

            m_fields->m_voteButton
                ->setVisible(canVote);
        }
    }

    void storeKnownMacro(
        std::string const& text,
        std::string_view source,
        bool isPublic,
        bool canonicalPublic = false,
        bool confirmedRemote = false
    ) {
        MacroData validationMacro;

        if (
            !deserializeMacro(
                text,
                validationMacro
            ) ||
            !isPlayableMacro(
                validationMacro
            )
        ) {
            log::warn(
                "Replay Gallery: Ignored {} macro with invalid "
                "or zero-only timing",
                source
            );

            return;
        }

        bool stored = true;

        if (isPublic) {
            // A confirmed canonical Firebase response is the
            // source of truth, even when it is lower than an old
            // cached replay left over from before a reset.
            if (confirmedRemote && canonicalPublic) {
                stored = true;
            }
            else if (!m_fields->m_publicMacro.empty()) {
                MacroData currentPublic;

                if (deserializeMacro(
                    m_fields->m_publicMacro,
                    currentPublic
                )) {
                    stored =
                        isBetterMacro(
                            validationMacro,
                            currentPublic
                        );
                }
            }

            if (stored) {
                m_fields->m_publicMacro =
                    text;

                m_fields->m_publicMacroCanonical =
                    canonicalPublic;

                m_fields->m_publicMacroETag.clear();
            }
        }
        else {
            m_fields->m_localMacro =
                text;
        }

        if (confirmedRemote) {
            m_fields->m_publicMacroFound = true;

            this->unschedule(
                schedule_selector(
                    ReplayGalleryLevelInfoLayer::
                        pollForPublicMacro
                )
            );
        }

        rebuildAvailableMacro();

        log::info(
            "Replay Gallery: {} run {} with {} inputs {}",
            source,
            formatRunResult(validationMacro),
            validationMacro.inputs.size(),
            stored
                ? "was stored"
                : "was below the known public run"
        );
    }

    void clearCanonicalPublicMacro() {
        auto* level =
            m_fields->m_level;

        if (level) {
            Mod::get()->setSavedValue<std::string>(
                remoteCacheKey(level),
                ""
            );
        }

        if (m_fields->m_publicMacroCanonical) {
            m_fields->m_publicMacro.clear();
            m_fields->m_publicMacroETag.clear();
            m_fields->m_publicMacroCanonical = false;
        }

        rebuildAvailableMacro();
    }

    void loadSavedAndCachedMacro() {
        auto* level =
            m_fields->m_level;

        if (!level) {
            return;
        }

        std::string localMacro =
            Mod::get()
                ->getSavedValue<std::string>(
                    localMacroKey(level),
                    ""
                );

        if (localMacro.empty()) {
            localMacro =
                Mod::get()
                    ->getSavedValue<std::string>(
                        legacyLocalMacroKey(level),
                        ""
                    );
        }

        if (!localMacro.empty()) {
            storeKnownMacro(
                localMacro,
                "local",
                false
            );
        }

        std::string cachedRemote =
            Mod::get()
                ->getSavedValue<std::string>(
                    remoteCacheKey(level),
                    ""
                );

        if (cachedRemote.empty()) {
            cachedRemote =
                Mod::get()
                    ->getSavedValue<std::string>(
                        legacyRemoteCacheKey(level),
                        ""
                    );
        }

        if (!cachedRemote.empty()) {
            storeKnownMacro(
                cachedRemote,
                "cached public",
                true,
                false
            );
        }
    }

    void pollForPublicMacro(float) {
        if (
            m_fields->m_publicMacroFound ||
            !m_fields->m_level
        ) {
            return;
        }

        if (m_fields->m_remoteRequest.isPending()) {
            return;
        }

        if (
            m_fields->m_publicCheckAttempts >= 10
        ) {
            this->unschedule(
                schedule_selector(
                    ReplayGalleryLevelInfoLayer::
                        pollForPublicMacro
                )
            );

            return;
        }

        auto* level =
            m_fields->m_level;

        bool useLegacyURL =
            m_fields->m_publicCheckAttempts == 1;

        std::string url =
            useLegacyURL
                ? legacyRemoteMacroURL(level)
                : remoteMacroURL(level);

        ++m_fields->m_publicCheckAttempts;

        auto request =
            web::WebRequest();

        request.userAgent(
            "Replay-Gallery/1.2.5"
        );

        request.header(
            "Cache-Control",
            "no-cache"
        );

        request.header(
            "Pragma",
            "no-cache"
        );

        request.header(
            "X-Firebase-ETag",
            "true"
        );

        request.timeout(
            std::chrono::seconds(4)
        );

        m_fields->m_remoteRequest.spawn(
            "Checking Firebase for a Replay Gallery macro",
            request.get(url),
            [
                this,
                useLegacyURL
            ](
                web::WebResponse response
            ) {
                if (!response.ok()) {
                    log::warn(
                        "Replay Gallery: Firebase macro "
                        "check returned HTTP {}",
                        response.code()
                    );

                    return;
                }

                std::string jsonBody =
                    response
                        .string()
                        .unwrapOr("");

                if (isJSONNull(jsonBody)) {
                    if (!useLegacyURL) {
                        clearCanonicalPublicMacro();
                    }

                    return;
                }

                std::string body;

                if (!decodeJSONString(
                    jsonBody,
                    body
                )) {
                    log::warn(
                        "Replay Gallery: Firebase returned "
                        "non-string macro data"
                    );

                    return;
                }

                MacroData validationMacro;

                if (
                    body.empty() ||
                    !deserializeMacro(
                        body,
                        validationMacro
                    ) ||
                    !isPlayableMacro(
                        validationMacro
                    )
                ) {
                    log::warn(
                        "Replay Gallery: Firebase had "
                        "macro data, but it was invalid"
                    );

                    return;
                }

                auto* currentLevel =
                    m_fields->m_level;

                if (
                    currentLevel &&
                    !useLegacyURL
                ) {
                    Mod::get()
                        ->setSavedValue<std::string>(
                            remoteCacheKey(currentLevel),
                            body
                        );
                }

                storeKnownMacro(
                    body,
                    useLegacyURL
                        ? "legacy Firebase public"
                        : "Firebase public",
                    true,
                    !useLegacyURL,
                    true
                );

                if (!useLegacyURL) {
                    m_fields->m_publicMacroETag =
                        firebaseETag(response);

                    rebuildAvailableMacro();
                }

                log::info(
                    "Replay Gallery: Downloaded public "
                    "macro with {} inputs",
                    validationMacro.inputs.size()
                );
            }
        );
    }
};
// ---------------------------------------------------------
// Attempt management and noclip detection
// ---------------------------------------------------------

class $modify(
    ReplayGalleryPlayLayer,
    PlayLayer
) {
public:
    struct Fields {
        CCLabelBMFont*
            m_statusLabel = nullptr;

        std::string m_lastStatus;

        bool m_resetting = false;
        bool m_wasDead = false;

        bool m_safePlayback = false;
        bool m_previousDontSave = false;

        unsigned int m_lastProgress = 0;
    };

    static void onModify(
        auto& self
    ) {
        if (!self.setHookPriorityPre(
            "PlayLayer::destroyPlayer",
            Priority::First
        )) {
            log::warn(
                "Replay Gallery: Could not set "
                "destroyPlayer hook priority"
            );
        }
    }

    bool init(
        GJGameLevel* level,
        bool useReplay,
        bool dontCreateObjects
    ) {
        if (!PlayLayer::init(
            level,
            useReplay,
            dontCreateObjects
        )) {
            return false;
        }

        clearActiveSession();

        g_activePlayLayer = this;

        g_feedbackLevelID =
            getLevelID(level);

        g_feedbackLevelVersion =
            getLevelVersion(level);

        g_attemptInvalid = false;
        g_attemptFinished = false;

        // Downloading a level may change the version value
        // between LevelInfoLayer and PlayLayer. The macro
        // was already selected for this exact level ID, so
        // match the pending playback by stable ID only.
        bool requestedPlayback =
            g_pendingPlayback &&
            level &&
            getLevelID(level) ==
                g_pendingLevelID;

        if (requestedPlayback) {
            g_currentMacro =
                std::move(
                    g_pendingMacro
                );

            g_playbackIndex = 0;

            clearPendingPlayback();

            bool validPlayback =
                isPlayableMacro(
                    g_currentMacro
                );

            g_playback =
                validPlayback;

            g_recording = false;

            if (validPlayback) {
                // Built-in Replay Gallery Safe Mode:
                // - Test mode keeps this run separate from
                //   an ordinary legitimate completion.
                // - m_dontSave prevents this level object
                //   from being written while the macro is
                //   active.
                // - GJGameLevel save hooks provide a final
                //   backstop if another mod calls a save
                //   method directly.
                m_fields->m_safePlayback = true;

                this->m_isTestMode = true;

                if (this->m_level) {
                    m_fields->m_previousDontSave =
                        this->m_level->m_dontSave;

                    this->m_level->m_dontSave =
                        true;
                }

                g_safePlaybackActive = true;

                log::info(
                    "Replay Gallery: Playback initialized "
                    "in Safe Mode with {} inputs through step {} "
                    "for level {} version {}",
                    g_currentMacro.inputs.size(),
                    g_currentMacro.endStep,
                    getLevelID(level),
                    getLevelVersion(level)
                );
            }
            else {
                log::error(
                    "Replay Gallery: Pending playback "
                    "was missing valid run data"
                );
            }
        }
        else {
            if (g_pendingPlayback) {
                log::warn(
                    "Replay Gallery: Pending playback "
                    "did not match the opened level"
                );

                clearPendingPlayback();
            }

            g_playback = false;
            g_safePlaybackActive = false;

            g_recording =
                !useReplay &&
                !this->m_isPracticeMode &&
                !this->m_isTestMode;

            g_currentMacro = {};
            g_playbackIndex = 0;

            if (g_recording) {
                log::info(
                    "Replay Gallery: "
                    "Input recording started"
                );
            }
        }

        g_attemptStep = 0;

        m_fields->m_wasDead =
            this->m_playerDied ||
            (
                this->m_player1 &&
                this->m_player1->m_isDead
            );

        m_fields->m_lastProgress =
            this->m_gameState
                .m_currentProgress;

        createStatusLabel();
        resetStatusText();

        return true;
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);

        if (
            g_activePlayLayer != this ||
            m_fields->m_resetting
        ) {
            return;
        }

        bool deadNow =
            this->m_playerDied ||
            (
                this->m_player1 &&
                this->m_player1->m_isDead
            );

        unsigned int progressNow =
            this->m_gameState
                .m_currentProgress;

        bool activeRecordingAttempt =
            g_recording &&
            !g_playback &&
            !g_attemptFinished &&
            this->m_started &&
            !this->m_levelEndAnimationStarted;

        if (
            activeRecordingAttempt &&
            !g_attemptInvalid
        ) {
            // Common noclip implementations disable the base-game
            // player hitbox. Reject the entire attempt as soon as
            // that flag is observed so it can never become a local
            // or public best run.
            if (this->m_disablePlayerHitbox) {
                invalidateAttempt(
                    "the player hitbox was disabled"
                );
            }
            else if (
                this->m_isPracticeMode ||
                this->m_isTestMode ||
                this->m_useReplay
            ) {
                // Recording is only started in normal mode. If a
                // mod switches the run into an unsafe mode after
                // the attempt begins, reject it as well.
                invalidateAttempt(
                    "the run switched out of normal mode"
                );
            }
        }

        // A failed best run has a recorded ending step. Normally the
        // same collision kills the replay naturally. This fallback
        // ends it at the stored step if another mod or a speedhack
        // changes collision callback ordering.
        if (
            g_playback &&
            !g_attemptFinished &&
            !g_currentMacro.completed &&
            g_currentMacro.endStep > 0 &&
            !deadNow &&
            macroPlaybackPosition(
                this,
                g_currentMacro
            ) >= g_currentMacro.endStep
        ) {
            g_attemptFinished = true;

            setStatus(
                fmt::format(
                    "REPLAY GALLERY: BEST RUN ENDED AT {}",
                    formatRunResult(
                        g_currentMacro
                    )
                )
            );

            if (this->m_player1) {
                PlayLayer::destroyPlayer(
                    this->m_player1,
                    nullptr
                );

                deadNow =
                    this->m_playerDied ||
                    this->m_player1->m_isDead;
            }
        }

        bool playerRespawned =
            m_fields->m_wasDead &&
            !deadNow;

        bool progressRestarted =
            progressNow + 4 <
            m_fields->m_lastProgress;

        // This catches restart paths used by the game or
        // other mods even when they bypass resetLevel().
        if (
            (
                playerRespawned ||
                progressRestarted
            ) &&
            !g_playback
        ) {
            beginNewAttempt(
                "respawn/progress reset"
            );
        }

        m_fields->m_wasDead =
            deadNow;

        m_fields->m_lastProgress =
            progressNow;
    }

    void destroyPlayer(
        PlayerObject* player,
        GameObject* object
    ) {
        bool realPlayer =
            player &&
            (
                player == this->m_player1 ||
                player == this->m_player2
            );

        bool activeRecording =
            g_recording &&
            g_activePlayLayer == this &&
            !g_attemptFinished &&
            !m_fields->m_resetting &&
            this->m_started &&
            !this->m_levelEndAnimationStarted;

        bool activePlayback =
            g_playback &&
            g_activePlayLayer == this &&
            !g_attemptFinished &&
            !m_fields->m_resetting;

        bool antiCheatCall =
            object &&
            object ==
                this->m_anticheatSpike;

        if (
            antiCheatCall ||
            !realPlayer ||
            (
                !activeRecording &&
                !activePlayback
            )
        ) {
            PlayLayer::destroyPlayer(
                player,
                object
            );

            return;
        }

        bool wasAlreadyDead =
            this->m_playerDied ||
            player->m_isDead;

        unsigned int deathPercent =
            currentPercentHundredths(this);

        PlayLayer::destroyPlayer(
            player,
            object
        );

        if (wasAlreadyDead) {
            return;
        }

        bool diedAfterCall =
            this->m_playerDied ||
            player->m_isDead;

        if (!diedAfterCall) {
            if (activeRecording) {
                invalidateAttempt();
            }

            return;
        }

        g_attemptFinished = true;
        m_fields->m_wasDead = true;

        if (activePlayback) {
            setStatus(
                fmt::format(
                    "REPLAY GALLERY: BEST RUN ENDED AT {}",
                    formatRunResult(
                        g_currentMacro
                    )
                )
            );

            log::info(
                "Replay Gallery: Best-run playback ended "
                "at target {}",
                formatRunResult(
                    g_currentMacro
                )
            );

            return;
        }

        if (g_attemptInvalid) {
            setStatus(
                "REPLAY GALLERY: INVALID ATTEMPT IGNORED"
            );

            return;
        }

        finishRecordedRun(
            false,
            deathPercent
        );
    }

    void resetLevel() {
        m_fields->m_resetting = true;

        PlayLayer::resetLevel();

        m_fields->m_resetting = false;

        beginNewAttempt(
            "resetLevel"
        );
    }

    void resetLevelFromStart() {
        m_fields->m_resetting = true;

        PlayLayer::resetLevelFromStart();

        m_fields->m_resetting = false;

        beginNewAttempt(
            "resetLevelFromStart"
        );
    }

    void fullReset() {
        m_fields->m_resetting = true;

        PlayLayer::fullReset();

        m_fields->m_resetting = false;

        beginNewAttempt(
            "fullReset"
        );
    }

    void levelComplete() {
        bool activeSession =
            g_activePlayLayer == this;

        bool recordingCompletion =
            activeSession &&
            g_recording;

        bool playbackCompletion =
            activeSession &&
            g_playback;

        if (activeSession) {
            g_attemptFinished = true;
        }

        if (playbackCompletion) {
            // Keep every Safe Mode guard enabled while the
            // original completion animation and end screen
            // are created.
            g_safePlaybackActive = true;
            this->m_isTestMode = true;

            if (this->m_level) {
                this->m_level->m_dontSave =
                    true;
            }

            setStatus(
                "REPLAY GALLERY: BEST RUN COMPLETE - "
                "SAFE MODE"
            );

            log::info(
                "Replay Gallery: Macro playback "
                "completed in Safe Mode"
            );

            PlayLayer::levelComplete();
            return;
        }

        if (recordingCompletion) {
            if (g_attemptInvalid) {
                setStatus(
                    "REPLAY GALLERY: NOCLIPPED ATTEMPT IGNORED"
                );

                log::warn(
                    "Replay Gallery: Completion was "
                    "not saved because a death was blocked"
                );
            }
            else if (!this->m_level) {
                setStatus(
                    "REPLAY GALLERY: LEVEL DATA WAS MISSING"
                );

                log::error(
                    "Replay Gallery: Could not save "
                    "because the level pointer was missing"
                );
            }
            else {
                finishRecordedRun(
                    true,
                    10000
                );
            }
        }

        PlayLayer::levelComplete();
    }

    void onExit() {
        GJGameLevel* level =
            this->m_level;

        bool restoreDontSave =
            m_fields->m_safePlayback &&
            level;

        bool previousDontSave =
            m_fields->m_previousDontSave;

        // Keep Safe Mode enabled through the original exit
        // routine in case Geometry Dash performs any final
        // save work while leaving the level.
        PlayLayer::onExit();

        if (restoreDontSave) {
            level->m_dontSave =
                previousDontSave;
        }

        if (
            g_activePlayLayer == this
        ) {
            clearActiveSession();
        }
    }

private:
    void beginNewAttempt(
        std::string_view reason
    ) {
        if (
            g_activePlayLayer != this
        ) {
            return;
        }

        g_attemptInvalid = false;
        g_attemptFinished = false;

        m_fields->m_wasDead = false;

        m_fields->m_lastProgress =
            this->m_gameState
                .m_currentProgress;

        g_attemptStep = 0;

        if (g_playback) {
            g_playbackIndex = 0;

            log::info(
                "Replay Gallery: Playback restarted "
                "through {}",
                reason
            );
        }
        else if (g_recording) {
            g_currentMacro = {};

            log::info(
                "Replay Gallery: New clean recording "
                "attempt started through {}",
                reason
            );
        }

        resetStatusText();
    }

    bool saveBestLocalRun(
        GJGameLevel* level,
        MacroData const& candidate,
        std::string const& serialized
    ) {
        if (!level || serialized.empty()) {
            return false;
        }

        std::string currentText =
            Mod::get()
                ->getSavedValue<std::string>(
                    localMacroKey(level),
                    ""
                );

        if (currentText.empty()) {
            currentText =
                Mod::get()
                    ->getSavedValue<std::string>(
                        legacyLocalMacroKey(level),
                        ""
                    );
        }

        if (!currentText.empty()) {
            MacroData currentBest;

            if (
                deserializeMacro(
                    currentText,
                    currentBest
                ) &&
                !isBetterMacro(
                    candidate,
                    currentBest
                )
            ) {
                return false;
            }
        }

        Mod::get()
            ->setSavedValue<std::string>(
                localMacroKey(level),
                serialized
            );

        std::filesystem::path exportedPath;

        if (exportMacro(
            level,
            serialized,
            exportedPath
        )) {
            log::info(
                "Replay Gallery: Best-run backup exported "
                "to {}",
                utils::string::pathToString(
                    exportedPath
                )
            );
        }

        return true;
    }

    void finishRecordedRun(
        bool completed,
        unsigned int percentHundredths
    ) {
        if (
            !g_recording ||
            g_activePlayLayer != this ||
            !this->m_level ||
            g_attemptInvalid
        ) {
            return;
        }

        g_currentMacro.usesSimulationSteps =
            true;

        g_currentMacro.completed =
            completed;

        g_currentMacro.endPercentHundredths =
            completed
                ? 10000
                : std::min(
                    percentHundredths,
                    10000u
                );

        g_currentMacro.endStep =
            relativeSimulationStep(this);

        MacroData candidate =
            g_currentMacro;

        std::string serialized =
            serializeMacro(candidate);

        bool newLocalBest =
            saveBestLocalRun(
                this->m_level,
                candidate,
                serialized
            );

        std::string result =
            formatRunResult(candidate);

        setStatus(
            newLocalBest
                ? fmt::format(
                    "REPLAY GALLERY: NEW BEST {} - CHECKING PUBLIC",
                    result
                )
                : fmt::format(
                    "REPLAY GALLERY: RUN {} - CHECKING PUBLIC",
                    result
                )
        );

        uploadPublicMacro(
            this->m_level,
            serialized,
            candidate
        );

        log::info(
            "Replay Gallery: Recorded {} run {} with {} "
            "inputs at relative step {}",
            completed ? "completed" : "failed",
            result,
            candidate.inputs.size(),
            candidate.endStep
        );
    }

    void uploadPublicMacro(
        GJGameLevel* level,
        std::string const& macroText,
        MacroData const& candidate
    ) {
        if (
            !level ||
            macroText.empty() ||
            !isPlayableMacro(candidate)
        ) {
            return;
        }

        int levelID =
            getLevelID(level);

        int levelVersion =
            getLevelVersion(level);

        std::string readURL =
            remoteMacroURL(level);

        std::string cachedKey =
            remoteCacheKey(level);

        auto setVisibleUploadStatus =
            [
                levelID,
                levelVersion
            ](
                std::string const& text
            ) {
                auto* playLayer =
                    PlayLayer::get();

                if (
                    !playLayer ||
                    !playLayer->m_level ||
                    getLevelID(
                        playLayer->m_level
                    ) != levelID ||
                    getLevelVersion(
                        playLayer->m_level
                    ) != levelVersion
                ) {
                    return;
                }

                auto* label =
                    typeinfo_cast<
                        CCLabelBMFont*
                    >(
                        playLayer
                            ->getChildByID(
                                "nonothenonokid."
                                "replay-gallery/"
                                "status-label"
                            )
                    );

                if (label) {
                    label->setString(
                        text.c_str()
                    );

                    auto windowSize =
                        CCDirector::sharedDirector()
                            ->getWinSize();

                    label->limitLabelWidth(
                        windowSize.width - 48.f,
                        0.20f,
                        0.13f
                    );
                }
            };

        auto compareRequest =
            web::WebRequest();

        compareRequest.userAgent(
            "Replay-Gallery/1.2.5"
        );

        compareRequest.header(
            "Cache-Control",
            "no-cache"
        );

        compareRequest.header(
            "Pragma",
            "no-cache"
        );

        compareRequest.header(
            "X-Firebase-ETag",
            "true"
        );

        compareRequest.timeout(
            std::chrono::seconds(8)
        );

        // First read the public run. Only a run that is farther than
        // the stored one is allowed to replace it.
        async::spawn(
            compareRequest.get(readURL),
            [
                levelID,
                levelVersion,
                readURL,
                cachedKey,
                macroText,
                candidate,
                setVisibleUploadStatus
            ](
                web::WebResponse response
            ) mutable {
                if (!response.ok()) {
                    if (
                        response.code() == 401 ||
                        response.code() == 403
                    ) {
                        setVisibleUploadStatus(
                            "REPLAY GALLERY: PUBLIC BEST CHECK "
                            "BLOCKED BY FIREBASE"
                        );
                    }
                    else {
                        setVisibleUploadStatus(
                            fmt::format(
                                "REPLAY GALLERY: PUBLIC BEST CHECK "
                                "FAILED (HTTP {})",
                                response.code()
                            )
                        );
                    }

                    log::warn(
                        "Replay Gallery: Could not compare the "
                        "public best for level {} (HTTP {}): {}",
                        levelID,
                        response.code(),
                        response.errorMessage()
                    );

                    return;
                }

                std::string jsonBody =
                    response
                        .string()
                        .unwrapOr("");

                std::string remoteText;
                MacroData remoteMacro;

                bool hasRemoteBest =
                    decodeJSONString(
                        jsonBody,
                        remoteText
                    ) &&
                    !remoteText.empty() &&
                    deserializeMacro(
                        remoteText,
                        remoteMacro
                    ) &&
                    isPlayableMacro(
                        remoteMacro
                    );

                std::string firebaseETag;

                for (auto const headerName : {
                    "ETag",
                    "Etag",
                    "etag"
                }) {
                    if (auto value = response.header(headerName)) {
                        firebaseETag.assign(
                            value->data(),
                            value->size()
                        );

                        if (!firebaseETag.empty()) {
                            break;
                        }
                    }
                }

                // Firebase documents null_etag as the conditional
                // token for an empty location. This fallback also
                // keeps first uploads working if a networking layer
                // drops the ETag response header for a JSON null.
                auto firstNonWhitespace =
                    jsonBody.find_first_not_of(
                        " \t\r\n"
                    );

                auto lastNonWhitespace =
                    jsonBody.find_last_not_of(
                        " \t\r\n"
                    );

                bool remoteLocationIsNull =
                    firstNonWhitespace !=
                        std::string::npos &&
                    lastNonWhitespace !=
                        std::string::npos &&
                    jsonBody.substr(
                        firstNonWhitespace,
                        lastNonWhitespace -
                            firstNonWhitespace + 1
                    ) == "null";

                if (
                    firebaseETag.empty() &&
                    remoteLocationIsNull
                ) {
                    firebaseETag =
                        "null_etag";
                }

                if (firebaseETag.empty()) {
                    setVisibleUploadStatus(
                        "REPLAY GALLERY: FIREBASE DID NOT RETURN "
                        "A BEST-RUN VERSION"
                    );

                    log::warn(
                        "Replay Gallery: Firebase did not return "
                        "an ETag for level {}",
                        levelID
                    );

                    return;
                }

                if (!remoteLocationIsNull && !hasRemoteBest) {
                    setVisibleUploadStatus(
                        "REPLAY GALLERY: PUBLIC REPLAY DATA IS "
                        "INVALID AND PROTECTED"
                    );

                    log::warn(
                        "Replay Gallery: Refusing to overwrite "
                        "non-null invalid public data for level {}",
                        levelID
                    );

                    return;
                }

                if (hasRemoteBest) {
                    if (isBetterMacro(candidate, remoteMacro)) {
                        setVisibleUploadStatus(
                            "REPLAY GALLERY: LOCAL BEST SAVED - "
                            "PUBLIC REPLAY IS PROTECTED UNTIL REMOVED"
                        );

                        log::info(
                            "Replay Gallery: Run {} beat the public "
                            "run {} for level {}, but the protected "
                            "public slot cannot be overwritten",
                            formatRunResult(candidate),
                            formatRunResult(remoteMacro),
                            levelID
                        );
                    }
                    else {
                        setVisibleUploadStatus(
                            fmt::format(
                                "REPLAY GALLERY: PUBLIC BEST REMAINS {}",
                                formatRunResult(remoteMacro)
                            )
                        );

                        log::info(
                            "Replay Gallery: Run {} did not beat "
                            "the public best {} for level {}",
                            formatRunResult(candidate),
                            formatRunResult(remoteMacro),
                            levelID
                        );
                    }

                    return;
                }

                std::string previousVoteID;

                auto uploadRequest =
                    web::WebRequest();

                uploadRequest.userAgent(
                    "Replay-Gallery/1.2.5"
                );

                uploadRequest.header(
                    "Content-Type",
                    "application/json"
                );

                uploadRequest.header(
                    "if-match",
                    firebaseETag
                );

                uploadRequest.timeout(
                    std::chrono::seconds(15)
                );

                uploadRequest.bodyString(
                    encodeJSONString(
                        macroText
                    )
                );

                // Firebase rejects print=silent when a conditional
                // if-match header is used. Send the PUT to the plain
                // .json URL so the ETag-protected upload is accepted.
                async::spawn(
                    uploadRequest.put(
                        readURL
                    ),
                    [
                        levelID,
                        levelVersion,
                        cachedKey,
                        macroText,
                        candidate,
                        previousVoteID,
                        setVisibleUploadStatus
                    ](
                        web::WebResponse uploadResponse
                    ) {
                        if (
                            uploadResponse.ok() ||
                            uploadResponse.code() == 204
                        ) {
                            Mod::get()
                                ->setSavedValue<std::string>(
                                    cachedKey,
                                    macroText
                                );

                            if (!previousVoteID.empty()) {
                                clearVotesForVoteID(
                                    levelID,
                                    previousVoteID
                                );
                            }

                            setVisibleUploadStatus(
                                fmt::format(
                                    "REPLAY GALLERY: NEW PUBLIC BEST {}",
                                    formatRunResult(candidate)
                                )
                            );

                            log::info(
                                "Replay Gallery: Uploaded new public "
                                "best {} for level {} version {}",
                                formatRunResult(candidate),
                                levelID,
                                levelVersion
                            );

                            return;
                        }

                        if (uploadResponse.code() == 412) {
                            std::string changedJSON =
                                uploadResponse
                                    .string()
                                    .unwrapOr("");

                            std::string changedText;
                            MacroData changedMacro;

                            bool hasChangedBest =
                                decodeJSONString(
                                    changedJSON,
                                    changedText
                                ) &&
                                !changedText.empty() &&
                                deserializeMacro(
                                    changedText,
                                    changedMacro
                                ) &&
                                isPlayableMacro(
                                    changedMacro
                                );

                            if (
                                hasChangedBest &&
                                !isBetterMacro(
                                    candidate,
                                    changedMacro
                                )
                            ) {
                                setVisibleUploadStatus(
                                    fmt::format(
                                        "REPLAY GALLERY: PUBLIC BEST IS NOW {}",
                                        formatRunResult(
                                            changedMacro
                                        )
                                    )
                                );
                            }
                            else {
                                setVisibleUploadStatus(
                                    "REPLAY GALLERY: PUBLIC BEST CHANGED - "
                                    "NEXT RUN WILL RETRY"
                                );
                            }

                            log::info(
                                "Replay Gallery: Conditional best-run "
                                "upload lost a simultaneous update for "
                                "level {}",
                                levelID
                            );

                            return;
                        }

                        if (
                            uploadResponse.code() == 401 ||
                            uploadResponse.code() == 403
                        ) {
                            setVisibleUploadStatus(
                                "REPLAY GALLERY: UPLOAD BLOCKED BY "
                                "FIREBASE RULES"
                            );
                        }
                        else {
                            setVisibleUploadStatus(
                                fmt::format(
                                    "REPLAY GALLERY: UPLOAD FAILED "
                                    "(HTTP {})",
                                    uploadResponse.code()
                                )
                            );
                        }

                        std::string responseBody =
                            uploadResponse
                                .string()
                                .unwrapOr("");

                        log::warn(
                            "Replay Gallery: Best-run upload failed "
                            "with HTTP {}: {}; response: {}",
                            uploadResponse.code(),
                            uploadResponse.errorMessage(),
                            responseBody
                        );
                    }
                );
            }
        );
    }

    void invalidateAttempt(
        std::string_view reason =
            "a death request was blocked while the player remained alive"
    ) {
        if (g_attemptInvalid) {
            return;
        }

        g_attemptInvalid = true;

        setStatus(
            "REPLAY GALLERY: NOCLIP DETECTED - NOT SAVING"
        );

        log::warn(
            "Replay Gallery: Noclip/unsafe attempt detected: {}",
            reason
        );
    }

    void createStatusLabel() {
        constexpr char STATUS_LABEL_ID[] =
            "nonothenonokid.replay-gallery/status-label";

        // A PlayLayer can occasionally be initialized again by
        // another mod or by a scene transition. Remove every old
        // Replay Gallery status node first so identical labels can
        // never stack on top of each other.
        for (int duplicate = 0; duplicate < 8; ++duplicate) {
            auto* oldStatus =
                this->getChildByID(
                    STATUS_LABEL_ID
                );

            if (!oldStatus) {
                break;
            }

            oldStatus->removeFromParentAndCleanup(
                true
            );
        }

        m_fields->m_statusLabel = nullptr;

        auto windowSize =
            CCDirector::sharedDirector()
                ->getWinSize();

        m_fields->m_statusLabel =
            CCLabelBMFont::create(
                "REPLAY GALLERY: STARTING",
                "bigFont.fnt"
            );

        if (!m_fields->m_statusLabel) {
            return;
        }

        // Keep the status well below the built-in progress,
        // percentage, and attempt labels. Long messages still
        // shrink automatically instead of running off-screen.
        m_fields->m_statusLabel
            ->setPosition({
                windowSize.width / 2.f,
                windowSize.height - 56.f
            });

        m_fields->m_statusLabel
            ->limitLabelWidth(
                windowSize.width - 48.f,
                0.20f,
                0.13f
            );

        m_fields->m_statusLabel
            ->setID(
                STATUS_LABEL_ID
            );

        this->addChild(
            m_fields->m_statusLabel,
            1000
        );
    }

    void resetStatusText() {
        if (g_playback) {
            setStatus(
                fmt::format(
                    "REPLAY GALLERY: PLAYING BEST RUN {} - "
                    "SAFE MODE",
                    formatRunResult(
                        g_currentMacro
                    )
                )
            );
        }
        else if (
            this->m_useReplay ||
            this->m_isPracticeMode ||
            this->m_isTestMode
        ) {
            setStatus(
                "REPLAY GALLERY: NOT RECORDING THIS MODE"
            );
        }
        else if (g_recording) {
            setStatus(
                "REPLAY GALLERY: RECORDING BEST RUN"
            );
        }
        else {
            setStatus(
                "REPLAY GALLERY: RECORDING DISABLED"
            );
        }
    }

    void setStatus(
        std::string const& text
    ) {
        if (
            m_fields->m_lastStatus ==
            text
        ) {
            return;
        }

        m_fields->m_lastStatus =
            text;

        if (
            m_fields->m_statusLabel
        ) {
            m_fields->m_statusLabel
                ->setString(
                    text.c_str()
                );

            auto windowSize =
                CCDirector::sharedDirector()
                    ->getWinSize();

            // Reset to the preferred size for every new
            // message, then shrink only when necessary.
            m_fields->m_statusLabel
                ->limitLabelWidth(
                    windowSize.width - 36.f,
                    0.22f,
                    0.14f
                );
        }
    }
};
