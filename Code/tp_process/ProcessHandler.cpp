
#include "ProcessHandler.h"

ProcessHandler::ProcessHandler() noexcept
    : OverlayRenderProcessHandler("skyrimtogether")
{
}

void ProcessHandler::OnContextCreated(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> context)
{
    OverlayRenderProcessHandler::OnContextCreated(browser, frame, context);
    if (!m_pCoreObject)
        return;

    m_pCoreObject->SetValue("on", CefV8Value::CreateFunction("on", m_pEventsHandler), V8_PROPERTY_ATTRIBUTE_NONE);
    m_pCoreObject->SetValue("off", CefV8Value::CreateFunction("off", m_pEventsHandler), V8_PROPERTY_ATTRIBUTE_NONE);

    m_pCoreObject->SetValue("connect", CefV8Value::CreateFunction("connect", m_pOverlayHandler), V8_PROPERTY_ATTRIBUTE_NONE);
    m_pCoreObject->SetValue("disconnect", CefV8Value::CreateFunction("disconnect", m_pOverlayHandler), V8_PROPERTY_ATTRIBUTE_NONE);
    m_pCoreObject->SetValue("sendMessage", CefV8Value::CreateFunction("sendMessage", m_pOverlayHandler), V8_PROPERTY_ATTRIBUTE_NONE);
    m_pCoreObject->SetValue("setTime", CefV8Value::CreateFunction("setTime", m_pOverlayHandler),V8_PROPERTY_ATTRIBUTE_NONE);
    m_pCoreObject->SetValue("deactivate", CefV8Value::CreateFunction("deactivate", m_pOverlayHandler), V8_PROPERTY_ATTRIBUTE_NONE);
    m_pCoreObject->SetValue("launchParty", CefV8Value::CreateFunction("launchParty", m_pOverlayHandler), V8_PROPERTY_ATTRIBUTE_NONE);
    m_pCoreObject->SetValue("leaveParty", CefV8Value::CreateFunction("leaveParty", m_pOverlayHandler), V8_PROPERTY_ATTRIBUTE_NONE);
    m_pCoreObject->SetValue("createPartyInvite", CefV8Value::CreateFunction("createPartyInvite", m_pOverlayHandler), V8_PROPERTY_ATTRIBUTE_NONE);
    m_pCoreObject->SetValue("acceptPartyInvite", CefV8Value::CreateFunction("acceptPartyInvite", m_pOverlayHandler), V8_PROPERTY_ATTRIBUTE_NONE);
    m_pCoreObject->SetValue("kickPartyMember", CefV8Value::CreateFunction("kickPartyMember", m_pOverlayHandler), V8_PROPERTY_ATTRIBUTE_NONE);
    m_pCoreObject->SetValue("changePartyLeader", CefV8Value::CreateFunction("changePartyLeader", m_pOverlayHandler), V8_PROPERTY_ATTRIBUTE_NONE);
    m_pCoreObject->SetValue("teleportToPlayer", CefV8Value::CreateFunction("teleportToPlayer", m_pOverlayHandler), V8_PROPERTY_ATTRIBUTE_NONE);
    m_pCoreObject->SetValue("toggleDebugUI", CefV8Value::CreateFunction("toggleDebugUI", m_pOverlayHandler), V8_PROPERTY_ATTRIBUTE_NONE);
    // Fallout 4 main menu multiplayer screens: mainMenuAction(action, jsonPayload)
    m_pCoreObject->SetValue("mainMenuAction", CefV8Value::CreateFunction("mainMenuAction", m_pOverlayHandler), V8_PROPERTY_ATTRIBUTE_NONE);
    // Fallout 4 build tools settings: buildTools(request, payload)
    m_pCoreObject->SetValue("buildTools", CefV8Value::CreateFunction("buildTools", m_pOverlayHandler), V8_PROPERTY_ATTRIBUTE_NONE);
    // Fallout 4, refused for our plugins: matchLoadOrder("host plugins, one per line"), then the game restarts
    m_pCoreObject->SetValue("matchLoadOrder", CefV8Value::CreateFunction("matchLoadOrder", m_pOverlayHandler), V8_PROPERTY_ATTRIBUTE_NONE);
    // Fallout 4 overlay buttons: the game's menu sounds, playSound("UIMenuOK")
    m_pCoreObject->SetValue("playSound", CefV8Value::CreateFunction("playSound", m_pOverlayHandler), V8_PROPERTY_ATTRIBUTE_NONE);
    // Fallout 4 server browsers, LAN tab: lanScan(), answered with the lanServers event
    m_pCoreObject->SetValue("lanScan", CefV8Value::CreateFunction("lanScan", m_pOverlayHandler), V8_PROPERTY_ATTRIBUTE_NONE);
    // Fallout 4 quest team-ups (Player manager > Quests): quests(request, payload)
    m_pCoreObject->SetValue("quests", CefV8Value::CreateFunction("quests", m_pOverlayHandler), V8_PROPERTY_ATTRIBUTE_NONE);
    // Fallout 4 adopted dogs, the name card: companions(request, payload)
    m_pCoreObject->SetValue("companions", CefV8Value::CreateFunction("companions", m_pOverlayHandler), V8_PROPERTY_ATTRIBUTE_NONE);
    // Fallout 4 settlement claims (Player manager > Settlements): settlements(request, payload)
    m_pCoreObject->SetValue("settlements", CefV8Value::CreateFunction("settlements", m_pOverlayHandler), V8_PROPERTY_ATTRIBUTE_NONE);
    // Fallout 4 nemeses (the questioning card; the nemeses themselves are in the Pip-Boy): nemeses(request, payload)
    m_pCoreObject->SetValue("nemeses", CefV8Value::CreateFunction("nemeses", m_pOverlayHandler), V8_PROPERTY_ATTRIBUTE_NONE);
    // Fallout 4 Steam friends, the social sidebar (join, invite, the host's answers): social(request, payload)
    m_pCoreObject->SetValue("social", CefV8Value::CreateFunction("social", m_pOverlayHandler), V8_PROPERTY_ATTRIBUTE_NONE);
    // Fallout 4 emotes, the wheel and the chat's commands: emotes(request, payload)
    m_pCoreObject->SetValue("emotes", CefV8Value::CreateFunction("emotes", m_pOverlayHandler), V8_PROPERTY_ATTRIBUTE_NONE);
    // Fallout 4 co-op dialogue, the vote card: dialogueVote(request, payload)
    m_pCoreObject->SetValue("dialogueVote", CefV8Value::CreateFunction("dialogueVote", m_pOverlayHandler), V8_PROPERTY_ATTRIBUTE_NONE);
}

void ProcessHandler::OnContextReleased(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> context)
{
    OverlayRenderProcessHandler::OnContextReleased(browser, frame, context);
}
