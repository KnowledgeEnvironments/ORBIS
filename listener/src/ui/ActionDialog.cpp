#include "ui/ActionDialog.hpp"

#include "platform/Platform.hpp"

#include "drive/Commands.hpp"

#include <algorithm>

namespace ke::ui {

namespace {

constexpr const char* kCreate = "principal.create";
constexpr const char* kMount = "workspace.mount";

std::string idPrefix8(const std::string& id) {
    return id.size() > 8 ? id.substr(0, 8) : id;
}

} // namespace

void ActionDialog::openCreatePrincipal(const std::string& self) {
    open_ = true;
    justOpened_ = true;
    twoStep_ = true;
    mountShown_ = false;
    self_ = self;
    title_ = "NEW PRINCIPAL";
    create_ = Step{};
    create_.action = kCreate;
    // Sensible starting values: a Route from the attached principal, a prefix Grant on the name
    // (filled in as the name is typed), every op but delegate (strictly narrower than root's).
    create_.values.values = json{{"routes-from", self},
                                 {"driven-by", "process"},
                                 {"grant", {{"scope", {{"prefix", ""}}}, {"ops", json::array({"read", "reference", "transmit", "write", "evaluate", "create"})}}}};
    mount_ = Step{};
    mount_.action = kMount;
}

void ActionDialog::openAction(const std::string& action, json defaults) {
    open_ = true;
    justOpened_ = true;
    twoStep_ = false;
    mountShown_ = false;
    title_ = upper(action);
    create_ = Step{};
    create_.action = action;
    create_.values = FormValues{};
    // defaults are applied once the specs exist (rebuildSpecs); keep them until then
    create_.values.values = std::move(defaults);
    mount_ = Step{};
}

void ActionDialog::openMount(const std::string& principal, const std::string& principalName) {
    open_ = true;
    justOpened_ = true;
    twoStep_ = false;
    title_ = "MOUNT WORKSPACE";
    create_ = Step{};
    create_.action = kMount;
    create_.values.values = json{{"principal", principal}, {"name", principalName}, {"shell", true}};
    mount_ = Step{};
}

void ActionDialog::close() {
    open_ = false;
}

void ActionDialog::rebuildSpecs(Step& s, const model::Actions& actions) {
    if (s.specsFrom == actions.applied()) {
        return;
    }
    s.specsFrom = actions.applied();
    const model::ActionInfo* a = actions.get(s.action);
    s.specs.clear();
    s.specsErr.clear();
    if (!a) {
        s.specsErr = "the world has no action/" + s.action + " yet";
        return;
    }
    if (a->schema.empty()) {
        s.specsErr = "action/" + s.action + " has no action/schema";
        return;
    }
    std::vector<ArgSpec> specs = specsFromSchema(a->schema, s.specsErr);
    if (specs.empty()) {
        return;
    }
    // Prefilled values were stored raw; re-apply them through the specs (lists, ints, choices).
    json raw = s.values.values.is_object() ? s.values.values : json::object();
    s.values = FormValues{};
    applyDefaults(specs, s.values, raw);
    s.specs = std::move(specs);
}

void ActionDialog::submit(Step& s, chat::Chat& chat) {
    const json args = argsFromValues(s.specs, s.values);
    s.sent = true;
    s.result = "sending…";
    s.resultErr = false;
    Step* step = &s;
    chat.actNamed(s.action, args, [step](const Response& r) {
        if (r.ok) {
            std::string line = "ok";
            if (r.result.contains("seq") && r.result["seq"].is_number()) {
                line += " seq " + std::to_string(r.result["seq"].get<long long>());
            } else {
                line += " seq " + std::to_string(r.seq);
            }
            for (const char* key : {"principal", "mount", "route", "object", "task"}) {
                if (r.result.contains(key) && r.result[key].is_string()) {
                    line += std::string("  ") + key + " " + idPrefix8(r.result[key].get<std::string>());
                }
            }
            if (r.result.contains("principal") && r.result["principal"].is_string()) {
                step->createdId = r.result["principal"].get<std::string>();
            }
            if (r.result.contains("name") && r.result["name"].is_string()) {
                step->createdName = r.result["name"].get<std::string>();
            }
            step->result = line;
            step->resultErr = false;
        } else {
            step->result = "err " + (r.err ? r.err->code + (r.err->message.empty() ? "" : ": " + r.err->message) : std::string("?"));
            step->resultErr = true;
            step->sent = false; // let the person fix the args and try again
        }
    });
}

FormContext ActionDialog::context(const model::Principals& principals, const std::string& scope) const {
    FormContext ctx;
    ctx.driveScope = scope;
    std::vector<std::pair<std::string, std::string>> models, adapters, routes, anyPrincipal;
    for (const model::PrincipalInfo* p : principals.ordered()) {
        const std::string label = principals.nameOf(p->id);
        if (p->cls == "model") {
            models.emplace_back(p->id, label);
        } else if (p->cls == "adapter") {
            adapters.emplace_back(p->id, label);
        }
        if (p->cls == "principal") {
            anyPrincipal.emplace_back(p->id, label);
        }
    }
    for (const auto& [id, r] : principals.routes()) {
        routes.emplace_back(id, principals.nameOf(id));
    }
    ctx.idChoices["model"] = models;
    ctx.idChoices["adapter"] = adapters;
    ctx.idChoices["routes-from"] = anyPrincipal;
    ctx.idChoices["principal"] = anyPrincipal;
    ctx.idChoices["route"] = routes;
    return ctx;
}

void ActionDialog::drawStep(Step& s, const model::Principals& principals, const model::Actions& actions, chat::Chat& chat,
                            const Session& session, SDL_Window* window, const char* submitWord, const char* driveName) {
    rebuildSpecs(s, actions);
    if (const model::ActionInfo* a = actions.get(s.action); a && !a->help.empty()) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
        text(Ink::secondary, a->help);
        ImGui::PopTextWrapPos();
        if (a->gated) {
            text(Ink::inspected, "gated: this action waits for an approval");
        }
    }
    if (s.action == "model.create") {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
        text(Ink::inspected, "key-env names an environment variable of the world's process: the key is read there at call time and never stored in the world or sent from here");
        ImGui::PopTextWrapPos();
    }
    if (s.specs.empty()) {
        text(Ink::secondary, s.specsErr.empty() ? "no arguments" : s.specsErr);
        if (!s.specsErr.empty()) {
            text(Ink::secondary, "// the dialog is built from action/schema; nothing to fill in until the world has it");
            return;
        }
    }
    const std::string scope = std::string("input:") + driveName + ":";
    const FormContext ctx = context(principals, scope);
    {
        std::lock_guard<std::mutex> lock(pickMutex_);
        if (!pickedPath_.empty()) {
            s.values.values["path"] = pickedPath_;
            pickedPath_.clear();
        }
    }
    if (s.action == kCreate) {
        // The model defaults to the attached principal's first Model once the fold knows it,
        // and the prefix follows the name until someone edits the prefix.
        if (!self_.empty()) {
            if (const model::PrincipalInfo* me = principals.get(self_); me && !me->models().empty()) {
                json* eng = valueSlot(s.values, "model");
                if (!eng->is_string() || eng->get<std::string>().empty()) {
                    *eng = me->models().front();
                }
            }
        }
        const json* name = valueSlot(s.values, "name");
        json* prefix = valueSlot(s.values, "grant.scope#1.prefix");
        if (name->is_string() && (!prefix->is_string() || prefix->get<std::string>().empty() || prefix->get<std::string>() == prefixFollowed_)) {
            *prefix = name->get<std::string>();
            prefixFollowed_ = name->get<std::string>();
        }
    }
    drawForm(s.specs, s.values, ctx);
    // A folder picker beside the path, where the platform has one.
    bool hasPath = false;
    for (const ArgSpec& a : s.specs) {
        hasPath = hasPath || a.name == "path";
    }
    if (hasPath && window && platform::canPickFolder()) {
        if (bracketButtonNamed("choose folder", std::string(driveName) + ":choose", !pickPending_)) {
            pickPending_ = true;
            platform::pickFolder(window, [this](const std::string& path) { // on the dialog's own thread
                std::lock_guard<std::mutex> lock(pickMutex_);
                pickPending_ = false;
                if (!path.empty()) {
                    pickedPath_ = path;
                }
            });
        }
        ImGui::SameLine();
        text(Ink::secondary, "// or type the path");
    }
    ImGui::Spacing();
    const bool canSend = session.attached() && !s.sent;
    if (bracketButtonNamed(submitWord, driveName, canSend, false, Ink::unresolved)) {
        submit(s, chat);
    }
    if (ImGui::IsItemHovered()) {
        const json preview = argsFromValues(s.specs, s.values);
        ImGui::SetTooltip("act {action \"%s\", args %s}", s.action.c_str(), preview.dump(1).c_str());
    }
    if (!s.result.empty()) {
        ImGui::SameLine();
        text(s.resultErr ? Ink::inspected : Ink::primary, s.result);
    }
}

void ActionDialog::draw(const model::Principals& principals, const model::Actions& actions, chat::Chat& chat,
                        const Session& session, ImFont* mono, SDL_Window* window) {
    if (!open_) {
        return;
    }
    ImGui::PushFont(mono, 0.0f);
    const char* popupId = "action-dialog";
    if (justOpened_) {
        ImGui::OpenPopup(popupId);
        justOpened_ = false;
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float cw = ImGui::CalcTextSize("0").x;
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(vp->WorkSize.x - 32.0f, cw * 96.0f), 0.0f), ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints(ImVec2(cw * 40.0f, 0.0f), ImVec2(vp->WorkSize.x - 32.0f, vp->WorkSize.y - 32.0f));
    bool keepOpen = true;
    if (ImGui::BeginPopupModal(popupId, &keepOpen, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize)) {
        paneTitle(title_.c_str());
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("[ CANCEL ]").x);
        if (bracketButtonNamed("cancel", "cancel")) {
            keepOpen = false;
        }
        ImGui::Separator();
        if (twoStep_) {
            text(Ink::strong, "1  principal.create");
            drawStep(create_, principals, actions, chat, session, window, "create", "create");
            ImGui::Separator();
            const bool created = !create_.createdId.empty();
            text(created ? Ink::strong : Ink::secondary, "2  workspace.mount");
            if (!created) {
                text(Ink::secondary, "// after the Principal exists: a directory as its mount and shell");
                if (bracketButtonNamed("mount workspace", "mount-step", false)) {
                }
            } else {
                if (!mountShown_) {
                    mountShown_ = true;
                    const json name = create_.values.values.contains("name") ? create_.values.values["name"] : json(create_.createdName);
                    mount_.values.values = json{{"principal", create_.createdId}, {"name", name}, {"shell", true}};
                    mount_.specsFrom = -2;
                }
                drawStep(mount_, principals, actions, chat, session, window, "mount workspace", "mount");
            }
            if (created && !mount_.result.empty() && !mount_.resultErr) {
                ImGui::Separator();
                text(Ink::primary, "done: the node appears in the graph as the events arrive");
                dialogGap();
                if (bracketButtonNamed("close", "close")) {
                    keepOpen = false;
                }
            }
        } else {
            drawStep(create_, principals, actions, chat, session, window, "run", "run");
            if (!create_.result.empty() && !create_.resultErr) {
                ImGui::SameLine();
                if (bracketButtonNamed("close", "close")) {
                    keepOpen = false;
                }
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::IsAnyItemActive()) {
            keepOpen = false;
        }
        drive::hit("pane:dialog", ImGui::GetWindowPos(), ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowSize().x,
                                                                    ImGui::GetWindowPos().y + ImGui::GetWindowSize().y));
        if (!keepOpen) {
            ImGui::CloseCurrentPopup();
            open_ = false;
        }
        ImGui::EndPopup();
    } else {
        open_ = false;
    }
    ImGui::PopFont();
}

} // namespace ke::ui
