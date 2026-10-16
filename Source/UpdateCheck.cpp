#include "UpdateCheck.h"
#include <juce_events/juce_events.h>

namespace kerf::updates
{

bool isNewer (const juce::String& candidate, const juce::String& current)
{
    auto parts = [] (const juce::String& v)
    {
        juce::StringArray a;
        a.addTokens (v.trim().trimCharactersAtStart ("vV"), ".", {});
        return a;
    };
    const auto a = parts (candidate), b = parts (current);
    for (int i = 0; i < juce::jmax (a.size(), b.size()); ++i)
    {
        const auto x = i < a.size() ? a[i].getIntValue() : 0;
        const auto y = i < b.size() ? b[i].getIntValue() : 0;
        if (x != y)
            return x > y;
    }
    return false;
}

juce::String installerSuffix()
{
   #if JUCE_MAC
    return "-macOS-Installer.pkg";
   #else
    return "-Windows-Setup.exe";
   #endif
}

Result parseFeed (const juce::String& text, const juce::String& current, const juce::String& installer)
{
    // Our own file { "version", "url" }, GitHub's latest release { "tag_name", "html_url" }, or
    // GitHub's list of releases (newest first; it has the pre-releases / betas, "latest" not).
    auto json = juce::JSON::parse (text);
    Result r;
    if (const auto* list = json.getArray(); list != nullptr)
    {
        if (list->isEmpty()) // the page is there, with nothing on it yet
        {
            r.status = Result::Status::notConfigured;
            return r;
        }
        json = list->getFirst();
    }
    r.version = json.getProperty ("version", json.getProperty ("tag_name", {})).toString().trimCharactersAtStart ("vV");
    // GitHub's "url" is its API address (a page of JSON): the release page is "html_url".
    const auto fromGitHub = json.hasProperty ("tag_name");
    r.downloadUrl = (fromGitHub ? json.getProperty ("html_url", {}) : json.getProperty ("url", {})).toString();
    if (const auto* assets = json.getProperty ("assets", {}).getArray(); fromGitHub && assets != nullptr && installer.isNotEmpty())
        for (const auto& asset : *assets)
            if (asset.getProperty ("name", {}).toString().endsWithIgnoreCase (installer))
                if (const auto file = asset.getProperty ("browser_download_url", {}).toString(); file.startsWith ("https://"))
                {
                    r.downloadUrl = file; // the installer itself, not a page to look for it on
                    break;
                }
    r.status = r.version.isEmpty() ? Result::Status::failed
                                   : (isNewer (r.version, current) ? Result::Status::newer : Result::Status::upToDate);
    return r;
}

namespace
{
    bool startCheckLaunched = false, startFoundNewer = false, startNoticed = false;
}

void checkOnceAtStart()
{
    if (startCheckLaunched)
        return;
    startCheckLaunched = true;
    checkAsync ([] (Result r) { startFoundNewer = r.status == Result::Status::newer; });
}

bool updateAvailable() { return startFoundNewer; }
bool updateNoticed()   { return startNoticed; }
void setUpdateNoticed() { startNoticed = true; }
void simulateStartResult (bool newer) { startCheckLaunched = true; startFoundNewer = newer; startNoticed = false; }

void checkAsync (std::function<void (Result)> onDone)
{
    if (juce::String (feedUrl).isEmpty())
    {
        Result r;
        r.status = Result::Status::notConfigured;
        juce::MessageManager::callAsync ([onDone, r] { onDone (r); });
        return;
    }

    juce::Thread::launch ([onDone]
    {
        Result r;
        // GitHub's API wants a User-Agent.
        const auto options = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                 .withConnectionTimeoutMs (6000)
                                 .withExtraHeaders ("User-Agent: KERF\r\nAccept: application/json");
        if (auto stream = juce::URL (feedUrl).createInputStream (options))
            r = parseFeed (stream->readEntireStreamAsString(), KERF_VERSION);
        juce::MessageManager::callAsync ([onDone, r] { onDone (r); });
    });
}

} // namespace kerf::updates
