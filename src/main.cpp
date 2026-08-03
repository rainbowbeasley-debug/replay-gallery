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
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

using namespace geode::prelude;

namespace {
    constexpr std::string_view FIREBASE_DATABASE_URL =
        "https://showcase-reborn-macros-default-rtdb."
        "firebaseio.com";

    constexpr std::string_view FEEDBACK_MOD_VERSION =
        "v1.0.0";

    int g_feedbackLevelID = 0;
    int g_feedbackLevelVersion = 0;

    bool g_feedbackSending = false;

    constexpr std::int64_t FEEDBACK_COOLDOWN_SECONDS =
        60 * 60;

    constexpr char FEEDBACK_LAST_SENT_KEY[] =
        "feedback-last-sent-unix-seconds-v1";

    struct MacroInput {
        unsigned int progress = 0;

        bool down = false;
        int button = 1;
        bool isPlayer1 = true;
    };

    struct MacroData {
        unsigned int endProgress = 0;

        std::vector<MacroInput> inputs;
    };

    PlayLayer* g_activePlayLayer = nullptr;

    bool g_recording = false;
    bool g_playback = false;
    bool g_injectingInput = false;

    bool g_attemptInvalid = false;
    bool g_attemptFinished = false;

    // True only while Showcase Reborn is playing a public
    // macro. It prevents the playback from saving progress,
    // scores, stars, coins, or a legitimate completion.
    bool g_safePlaybackActive = false;

    MacroData g_currentMacro;
    std::size_t g_playbackIndex = 0;

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
            "{}-{}.showcase",
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
            return "showcase-macro-v6-invalid";
        }

        return fmt::format(
            "showcase-macro-v6-{}",
            getLevelID(level)
        );
    }

    // Read old saves so existing recordings are not lost.
    std::string legacyLocalMacroKey(
        GJGameLevel* level
    ) {
        if (!level) {
            return "showcase-macro-v5-invalid";
        }

        return fmt::format(
            "showcase-macro-v5-{}-{}",
            getLevelID(level),
            getLevelVersion(level)
        );
    }

    std::string remoteCacheKey(
        GJGameLevel* level
    ) {
        if (!level) {
            return "showcase-firebase-cache-v2-invalid";
        }

        return fmt::format(
            "showcase-firebase-cache-v2-{}",
            getLevelID(level)
        );
    }

    std::string legacyRemoteCacheKey(
        GJGameLevel* level
    ) {
        if (!level) {
            return "showcase-firebase-cache-v1-invalid";
        }

        return fmt::format(
            "showcase-firebase-cache-v1-{}-{}",
            getLevelID(level),
            getLevelVersion(level)
        );
    }

    std::string remoteMacroURL(
        GJGameLevel* level
    ) {
        return fmt::format(
            "{}/macros/{}.json",
            FIREBASE_DATABASE_URL,
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

    std::string cacheBustedURL(
        std::string const& url
    ) {
        auto stamp =
            std::chrono::duration_cast<
            std::chrono::milliseconds
            >(
                std::chrono::steady_clock::now()
                .time_since_epoch()
            ).count();

        return fmt::format(
            "{}?cb={}",
            url,
            stamp
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

    std::string serializeMacro(
        MacroData const& macro
    ) {
        std::ostringstream output;

        output
            << "SHOWCASE_REBORN_MACRO_V1\n";

        output
            << macro.endProgress
            << "\n";

        output
            << macro.inputs.size()
            << "\n";

        for (
            MacroInput const& input :
            macro.inputs
            ) {
            output
                << input.progress
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

        if (
            header !=
            "SHOWCASE_REBORN_MACRO_V1"
            ) {
            return false;
        }

        MacroData loadedMacro;

        std::size_t inputCount = 0;

        if (!(
            inputStream >>
            loadedMacro.endProgress
            )) {
            return false;
        }

        if (!(
            inputStream >>
            inputCount
            )) {
            return false;
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
                macroInput.progress >>
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
                        first.progress <
                        second.progress;
            }
        );

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
                "Showcase Reborn: Could not create "
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
                "Showcase Reborn: Could not open the "
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
                "Showcase Reborn: Writing the macro "
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
}

// ---------------------------------------------------------
// Suggestion and bug-report popup
// ---------------------------------------------------------

class ShowcaseFeedbackPopup :
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
            "Showcase Reborn Feedback"
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
                    ShowcaseFeedbackPopup::
                    onBug
                )
            );

        bugButton->setPosition({
            115.f,
            164.f
            });

        bugButton->setID(
            "nonothenonokid.showcase-reborn/"
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
                    ShowcaseFeedbackPopup::
                    onSuggestion
                )
            );

        suggestionButton->setPosition({
            265.f,
            164.f
            });

        suggestionButton->setID(
            "nonothenonokid.showcase-reborn/"
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
            "nonothenonokid.showcase-reborn/"
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
            "nonothenonokid.showcase-reborn/"
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
                    ShowcaseFeedbackPopup::
                    onSubmit
                )
            );

        submitButton->setPosition({
            190.f,
            35.f
            });

        submitButton->setID(
            "nonothenonokid.showcase-reborn/"
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
            "Showcase-Reborn/1.0.2"
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
                            "Showcase Reborn: Feedback "
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
                        "Showcase Reborn: Feedback upload "
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
    static ShowcaseFeedbackPopup* create() {
        auto* result =
            new ShowcaseFeedbackPopup();

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
    ShowcaseMenuLayer,
    MenuLayer
) {
public:
    bool init() {
        if (!MenuLayer::init()) {
            return false;
        }

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
            "nonothenonokid.showcase-reborn/"
            "feedback-menu"
        );

        auto* feedbackSprite =
            ButtonSprite::create(
                "Showcase Reborn Feedback",
                0.48f
            );

        auto* feedbackButton =
            CCMenuItemSpriteExtra::create(
                feedbackSprite,
                this,
                menu_selector(
                    ShowcaseMenuLayer::
                    onFeedback
                )
            );

        feedbackButton->setPosition({
            windowSize.width - 105.f,
            windowSize.height - 25.f
            });

        feedbackButton->setID(
            "nonothenonokid.showcase-reborn/"
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
            ShowcaseFeedbackPopup::create();

        if (popup) {
            popup->show();
        }
    }
};

// ---------------------------------------------------------
// Built-in Showcase Safe Mode
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
    ShowcaseSafeGameLevel,
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
                "Showcase Reborn: Safe Mode blocked "
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
                "Showcase Reborn: Safe Mode blocked "
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
    ShowcaseBaseGameLayer,
    GJBaseGameLayer
) {
public:
    void handleButton(
        bool down,
        int button,
        bool isPlayer1
    ) {
        // Do not let the viewer's clicks interfere with a
        // downloaded Showcase macro. Injected macro inputs
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

        recordedInput.progress =
            this->m_gameState
            .m_currentProgress;

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
                previous.progress ==
                recordedInput.progress &&
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
        bool shouldPlay =
            g_playback &&
            !g_attemptFinished &&
            isActiveGameLayer(this);

        if (shouldPlay) {
            unsigned int progress =
                this->m_gameState
                .m_currentProgress;

            while (
                g_playbackIndex <
                g_currentMacro.inputs.size()
                ) {
                MacroInput const& input =
                    g_currentMacro.inputs[
                        g_playbackIndex
                    ];

                if (
                    input.progress >
                    progress
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
    }
};

// ---------------------------------------------------------
// Automatic Firebase Showcase button
// ---------------------------------------------------------

class $modify(
    ShowcaseLevelInfoLayer,
    LevelInfoLayer
) {
public:
    struct Fields {
        GJGameLevel* m_level = nullptr;

        CCMenuItemSpriteExtra*
            m_showcaseButton = nullptr;

        std::string m_availableMacro;

        async::TaskHolder<
            web::WebResponse
        > m_remoteRequest;

        int m_publicCheckAttempts = 0;
        bool m_publicMacroFound = false;
    };

    bool init(
        GJGameLevel * level,
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
                "Showcase Reborn: "
                "Could not find play-menu"
            );

            return true;
        }

        auto* icon =
            CCSprite::createWithSpriteFrameName(
                "GJ_searchBtn_001.png"
            );

        if (!icon) {
            log::error(
                "Showcase Reborn: "
                "Could not create button icon"
            );

            return true;
        }

        icon->setScale(
            0.38f
        );

        auto* button =
            CCMenuItemSpriteExtra::create(
                icon,
                this,
                menu_selector(
                    ShowcaseLevelInfoLayer::
                    onShowcaseButton
                )
            );

        button->setPosition({
            40.f,
            20.f
            });

        button->setID(
            "nonothenonokid.showcase-reborn/"
            "showcase-button"
        );

        button->setVisible(false);

        playMenu->addChild(
            button
        );

        m_fields->m_showcaseButton =
            button;

        loadSavedAndCachedMacro();

        // Check immediately, then keep checking for a short
        // time. This covers the common case where a player
        // returns to the level page before their upload has
        // finished reaching Firebase.
        pollForPublicMacro(0.f);

        this->schedule(
            schedule_selector(
                ShowcaseLevelInfoLayer::
                pollForPublicMacro
            ),
            1.f
        );

        return true;
    }

    void onShowcaseButton(
        CCObject*
    ) {
        auto* level =
            m_fields->m_level;

        if (!level) {
            FLAlertLayer::create(
                "Showcase Reborn",
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
                "Showcase Reborn",
                "The available macro could not "
                "be read.",
                "OK"
            )->show();

            return;
        }

        if (
            loadedMacro.endProgress == 0
            ) {
            FLAlertLayer::create(
                "Showcase Reborn",
                "The macro exists, but its completion "
                "progress is missing. Record the level "
                "again with the newest build.",
                "OK"
            )->show();

            return;
        }

        if (m_fields->m_showcaseButton) {
            m_fields->m_showcaseButton
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
            "Showcase Reborn: Starting available "
            "macro with {} inputs for level {} "
            "version {}",
            g_pendingMacro.inputs.size(),
            g_pendingLevelID,
            g_pendingLevelVersion
        );

        // Use Geometry Dash's normal Play button path.
        // This downloads the level first when the local
        // level data is missing, then our PlayLayer hook
        // consumes the pending public macro.
        LevelInfoLayer::onPlay(
            m_fields->m_showcaseButton
        );
    }

private:
    void makeMacroAvailable(
        std::string const& text,
        std::string_view source,
        bool confirmedRemote = false
    ) {
        MacroData validationMacro;

        if (!deserializeMacro(
            text,
            validationMacro
        )) {
            return;
        }

        m_fields->m_availableMacro =
            text;

        if (confirmedRemote) {
            m_fields->m_publicMacroFound = true;

            this->unschedule(
                schedule_selector(
                    ShowcaseLevelInfoLayer::
                    pollForPublicMacro
                )
            );
        }

        if (m_fields->m_showcaseButton) {
            m_fields
                ->m_showcaseButton
                ->setVisible(true);
        }

        log::info(
            "Showcase Reborn: {} macro is "
            "available with {} inputs",
            source,
            validationMacro.inputs.size()
        );
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
            makeMacroAvailable(
                localMacro,
                "local"
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

        if (
            m_fields
            ->m_availableMacro
            .empty() &&
            !cachedRemote.empty()
            ) {
            makeMacroAvailable(
                cachedRemote,
                "cached public"
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

        // Do not cancel an in-flight request by spawning a
        // replacement request into the same TaskHolder.
        if (m_fields->m_remoteRequest.isPending()) {
            return;
        }

        // Ten actual requests gives Firebase enough time to
        // receive an upload made just before this page opened.
        if (
            m_fields->m_publicCheckAttempts >= 10
            ) {
            this->unschedule(
                schedule_selector(
                    ShowcaseLevelInfoLayer::
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
            "Showcase-Reborn/1.0.4"
        );

        request.header(
            "Cache-Control",
            "no-cache"
        );

        request.timeout(
            std::chrono::seconds(4)
        );

        m_fields->m_remoteRequest.spawn(
            "Checking Firebase for a Showcase macro",
            request.get(
                cacheBustedURL(url)
            ),
            [
                this,
                useLegacyURL
            ](
                web::WebResponse response
                ) {
                    if (!response.ok()) {
                        log::warn(
                            "Showcase Reborn: Firebase macro "
                            "check returned HTTP {}",
                            response.code()
                        );

                        return;
                    }

                    std::string jsonBody =
                        response
                        .string()
                        .unwrapOr("");

                    std::string body;

                    if (!decodeJSONString(
                        jsonBody,
                        body
                    )) {
                        // Firebase returns JSON null when no
                        // macro exists at this location yet.
                        return;
                    }

                    MacroData validationMacro;

                    if (
                        body.empty() ||
                        !deserializeMacro(
                            body,
                            validationMacro
                        ) ||
                        validationMacro.endProgress == 0
                        ) {
                        log::warn(
                            "Showcase Reborn: Firebase had "
                            "macro data, but it was invalid"
                        );

                        return;
                    }

                    auto* level =
                        m_fields->m_level;

                    if (level) {
                        Mod::get()
                            ->setSavedValue<std::string>(
                                remoteCacheKey(level),
                                body
                            );
                    }

                    makeMacroAvailable(
                        body,
                        useLegacyURL
                        ? "legacy Firebase public"
                        : "Firebase public",
                        true
                    );

                    log::info(
                        "Showcase Reborn: Downloaded public "
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
    ShowcasePlayLayer,
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
                "Showcase Reborn: Could not set "
                "destroyPlayer hook priority"
            );
        }
    }

    bool init(
        GJGameLevel * level,
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
                g_currentMacro.endProgress > 0;

            g_playback =
                validPlayback;

            g_recording = false;

            if (validPlayback) {
                // Built-in Showcase Safe Mode:
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
                    "Showcase Reborn: Playback initialized "
                    "in Safe Mode with {} inputs for "
                    "level {} version {}",
                    g_currentMacro.inputs.size(),
                    getLevelID(level),
                    getLevelVersion(level)
                );
            }
            else {
                log::error(
                    "Showcase Reborn: Pending playback "
                    "was missing completion progress"
                );
            }
        }
        else {
            if (g_pendingPlayback) {
                log::warn(
                    "Showcase Reborn: Pending playback "
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
                    "Showcase Reborn: "
                    "Input recording started"
                );
            }
        }

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
        PlayerObject * player,
        GameObject * object
    ) {
        bool realPlayer =
            player &&
            (
                player == this->m_player1 ||
                player == this->m_player2
                );

        bool recordableAttempt =
            g_recording &&
            g_activePlayLayer == this &&
            !g_attemptFinished &&
            !m_fields->m_resetting &&
            this->m_started &&
            !this->m_levelEndAnimationStarted;

        bool antiCheatCall =
            object &&
            object ==
            this->m_anticheatSpike;

        if (antiCheatCall) {
            PlayLayer::destroyPlayer(
                player,
                object
            );

            return;
        }

        if (
            !realPlayer ||
            !recordableAttempt
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

        if (diedAfterCall) {
            // End this attempt immediately so extra death
            // callbacks during the death animation cannot
            // leak into the next attempt.
            g_attemptFinished = true;
            g_currentMacro = {};

            m_fields->m_wasDead = true;

            setStatus(
                "SHOWCASE: DEATH DETECTED - "
                "ATTEMPT DISCARDED"
            );

            log::info(
                "Showcase Reborn: Normal death ended "
                "the current recording attempt"
            );

            return;
        }

        invalidateAttempt();
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
                "SHOWCASE: PUBLIC MACRO COMPLETE - "
                "SAFE MODE"
            );

            log::info(
                "Showcase Reborn: Macro playback "
                "completed in Safe Mode"
            );

            PlayLayer::levelComplete();
            return;
        }

        if (recordingCompletion) {
            if (g_attemptInvalid) {
                setStatus(
                    "SHOWCASE: NOCLIPPED ATTEMPT IGNORED"
                );

                log::warn(
                    "Showcase Reborn: Completion was "
                    "not saved because a death was blocked"
                );
            }
            else if (!this->m_level) {
                setStatus(
                    "SHOWCASE: LEVEL DATA WAS MISSING"
                );

                log::error(
                    "Showcase Reborn: Could not save "
                    "because the level pointer was missing"
                );
            }
            else {
                g_currentMacro.endProgress =
                    this->m_gameState
                    .m_currentProgress;

                std::string savedData =
                    serializeMacro(
                        g_currentMacro
                    );

                Mod::get()
                    ->setSavedValue<std::string>(
                        localMacroKey(
                            this->m_level
                        ),
                        savedData
                    );

                std::filesystem::path
                    exportedPath;

                if (exportMacro(
                    this->m_level,
                    savedData,
                    exportedPath
                )) {
                    log::info(
                        "Showcase Reborn: Backup exported "
                        "to {}",
                        utils::string::pathToString(
                            exportedPath
                        )
                    );
                }

                setStatus(
                    fmt::format(
                        "SHOWCASE: SAVED {} INPUTS - "
                        "UPLOADING",
                        g_currentMacro
                        .inputs
                        .size()
                    )
                );

                uploadPublicMacro(
                    this->m_level,
                    savedData
                );

                log::info(
                    "Showcase Reborn: Saved {} inputs "
                    "for level {} version {}",
                    g_currentMacro.inputs.size(),
                    getLevelID(
                        this->m_level
                    ),
                    getLevelVersion(
                        this->m_level
                    )
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

        if (g_playback) {
            g_playbackIndex = 0;

            log::info(
                "Showcase Reborn: Playback restarted "
                "through {}",
                reason
            );
        }
        else if (g_recording) {
            g_currentMacro = {};

            log::info(
                "Showcase Reborn: New clean recording "
                "attempt started through {}",
                reason
            );
        }

        resetStatusText();
    }

    void uploadPublicMacro(
        GJGameLevel * level,
        std::string const& macroText
    ) {
        if (
            !level ||
            macroText.empty()
            ) {
            return;
        }

        int levelID =
            getLevelID(level);

        int levelVersion =
            getLevelVersion(level);

        std::string url =
            remoteMacroURL(level) +
            "?print=silent";

        auto request =
            web::WebRequest();

        request.userAgent(
            "Showcase-Reborn/1.0.4"
        );

        request.header(
            "Content-Type",
            "application/json"
        );

        request.timeout(
            std::chrono::seconds(15)
        );

        request.bodyString(
            encodeJSONString(
                macroText
            )
        );

        // This upload must outlive PlayLayer. A TaskHolder
        // stored on PlayLayer is destroyed when the player
        // leaves the level, which cancels slow uploads.
        async::spawn(
            request.put(url),
            [
                levelID,
                levelVersion
            ](
                web::WebResponse response
                ) {
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
                                            "showcase-reborn/"
                                            "status-label"
                                        )
                                    );

                                if (label) {
                                    label->setString(
                                        text.c_str()
                                    );
                                }
                        };

                    if (
                        response.ok() ||
                        response.code() == 204
                        ) {
                        setVisibleUploadStatus(
                            "SHOWCASE: PUBLIC MACRO UPLOADED"
                        );

                        log::info(
                            "Showcase Reborn: Uploaded public "
                            "macro for level {} version {}",
                            levelID,
                            levelVersion
                        );

                        return;
                    }

                    if (
                        response.code() == 401 ||
                        response.code() == 403
                        ) {
                        setVisibleUploadStatus(
                            "SHOWCASE: UPLOAD BLOCKED BY "
                            "FIREBASE RULES"
                        );

                        log::warn(
                            "Showcase Reborn: Firebase rules "
                            "blocked the upload for level {} "
                            "version {}",
                            levelID,
                            levelVersion
                        );

                        return;
                    }

                    setVisibleUploadStatus(
                        fmt::format(
                            "SHOWCASE: UPLOAD FAILED "
                            "(HTTP {})",
                            response.code()
                        )
                    );

                    log::warn(
                        "Showcase Reborn: Firebase upload "
                        "failed with HTTP {}: {}",
                        response.code(),
                        response.errorMessage()
                    );
            }
        );
    }

    void invalidateAttempt() {
        if (g_attemptInvalid) {
            return;
        }

        g_attemptInvalid = true;

        setStatus(
            "SHOWCASE: SHOULD HAVE DIED - NOT SAVING"
        );

        log::warn(
            "Showcase Reborn: A death request was "
            "blocked while the player remained alive"
        );
    }

    void createStatusLabel() {
        auto windowSize =
            CCDirector::sharedDirector()
            ->getWinSize();

        m_fields->m_statusLabel =
            CCLabelBMFont::create(
                "SHOWCASE: STARTING",
                "bigFont.fnt"
            );

        if (!m_fields->m_statusLabel) {
            return;
        }

        m_fields->m_statusLabel
            ->setScale(
                0.27f
            );

        m_fields->m_statusLabel
            ->setPosition({
                windowSize.width / 2.f,
                windowSize.height - 12.f
                });

        m_fields->m_statusLabel
            ->setID(
                "nonothenonokid.showcase-reborn/"
                "status-label"
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
                    "SHOWCASE: PLAYING PUBLIC MACRO - "
                    "SAFE MODE ({} INPUTS)",
                    g_currentMacro.inputs.size()
                )
            );
        }
        else if (
            this->m_useReplay ||
            this->m_isPracticeMode ||
            this->m_isTestMode
            ) {
            setStatus(
                "SHOWCASE: NOT RECORDING THIS MODE"
            );
        }
        else if (g_recording) {
            setStatus(
                "SHOWCASE: RECORDING CLEAN ATTEMPT"
            );
        }
        else {
            setStatus(
                "SHOWCASE: RECORDING DISABLED"
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
        }
    }
};