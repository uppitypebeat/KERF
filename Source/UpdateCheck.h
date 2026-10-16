#pragma once

#include <juce_core/juce_core.h>
#include <functional>

/** "Check for updates": reads a small JSON file from the release page,
        { "version": "0.6.0", "url": "https://.../download" }
    and compares it with this build. Only when asked (the menu), never in the background. */
namespace kerf::updates
{
    /** Where the latest version is published: KERF's GitHub releases, newest first (betas
        included; ".../releases/latest" would skip pre-releases). Empty: the check says there is
        no update page instead of failing. */
    inline constexpr const char* feedUrl = "https://api.github.com/repos/uppitypebeat/KERF/releases?per_page=1";

    /** "0.10.0" > "0.9.2": numeric, part by part (missing parts count as 0). */
    bool isNewer (const juce::String& candidate, const juce::String& current);

    struct Result
    {
        enum class Status { upToDate, newer, notConfigured, failed } status = Status::failed;
        juce::String version, downloadUrl;
    };

    /** The installer for this computer, as the release names it (its name ends with this):
        "-Windows-Setup.exe" or "-macOS-Installer.pkg". */
    juce::String installerSuffix();

    /** The answer from the feed: our own file { "version", "url" } or GitHub's latest release
        (api.github.com/repos/OWNER/REPO/releases/latest: "tag_name", "html_url"). From GitHub,
        DOWNLOAD fetches the installer for this computer straight away (the release's file
        whose name ends with `installer`); without one, it opens the release page. */
    Result parseFeed (const juce::String& json, const juce::String& currentVersion,
                      const juce::String& installer = installerSuffix());

    /** Reads the feed on a background thread; `onDone` runs on the message thread. */
    void checkAsync (std::function<void (Result)> onDone);

    /** The check when KERF opens: once per run of the DAW (every KERF window shares its
        answer). Message thread only, like the two below. */
    void checkOnceAtStart();
    /** That check found a newer version (false until it is known, or offline). */
    bool updateAvailable();
    /** The menu has been opened since: the menu button stops blinking (the dot by "Check for
        updates" stays while there is an update). */
    bool updateNoticed();
    void setUpdateNoticed();
    /** For the screenshot tool: as if the check at start had answered (no network). */
    void simulateStartResult (bool newer);
}
