// A terminal per Principal: slanted tabs across the top (one per open Principal, Cmd-W closes),
// then three views for the current one:
//   TRANSCRIPT  its Messages as a conversation (FROM  hh:mm:ss  [via eval]), our lines marked
//               `>` in white, the loop's stalled reason in red, a spinner while it evaluates,
//               and the input line that `send`s to its first inbox route (NO ROUTE disables it)
//   PROMPT      its instruction Fields (`instructions/*`) in multiline boxes, [ SET ] writes the
//               same Field back with `set-field`; a NEW FIELD row adds one
//   MEMORY      memory/notes rendered, [ EDIT ] writes it back; memory/policy, memory/budget,
//               memory/retention-days editable; the notes' size against the budget as a bar;
//               [ CREATE DEFAULTS ] sets the four Fields when the Principal has none yet
//   FIELDS      every Field (name, type, value, by, at) with inline edit -> `set-field`; a HINT
//               column (`<field>/hint`, one line, with a reader selector) and a comment thread per
//               Field (`<field>/comment`, a list of {by at text}); the object's own `object/hint`
//               and `object/comment` at the top. Hints are what a person or a Principal tells
//               the next reader about a Field; the serializer renders them beside the value.
// Nothing here is special to root: with another token the world answers err and that shows.
#pragma once

#include "model/Activity.hpp"
#include "ui/TabBar.hpp"

#include "chat/Chat.hpp"
#include "chat/InputLine.hpp"
#include "chat/KernelRecord.hpp"
#include "chat/Notifications.hpp"
#include "chat/Conversations.hpp"
#include "chat/PromptSections.hpp"
#include "ui/RuleForm.hpp"
#include "chat/Queue.hpp"
#include "chat/Transcript.hpp"
#include "model/Evaluations.hpp"
#include "model/Principals.hpp"
#include "model/Actions.hpp"
#include "model/Tasks.hpp"
#include "model/Tokens.hpp"
#include "net/Session.hpp"
#include "ui/InputPane.hpp"
#include "ui/Markdown.hpp"
#include "ui/SelectableText.hpp"
#include "ui/Theme.hpp"
#include "ui/ViewBlocks.hpp"

#include <functional>
#include <imgui.h>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace ke::ui {

class TerminalPane {
  public:
    // Adds a tab when needed and makes it current; `view` 0 transcript, 1 prompt, 3 memory, -1 keep.
    const char* windowName = "CHAT"; // the inspector is a second one of these, in its own window
    // View 2 asks for the object's inspector: its own tab, apart from an agent's chat.
    void open(const std::string& principal, int view = -1);
    // Bring this Message into view in its agent's chat: unfolds what hides it, scrolls, marks it briefly.
    void jumpTo(const std::string& principal, const std::string& message);
    const std::string& lastJump() const { return lastJump_; }
    // An object by name (a mounted file): read, then opened in its inspector tab.
    void openNamed(const std::string& name) { pendingNames_.push_back(name); }
    // FIELDS of `object`, scrolled to `field` on the next frame.
    void scrollToField(const std::string& object, const std::string& field);
    // SHOW REASONING (SETTINGS menu, the status bar's [ SHOW REASONING ] bracket, persisted by the app):
    // evaluation/thinking as a collapsed dim block above a reply.
    bool showThinking = false;
    // The transcript filter (persisted by the app): "all" shows heartbeats and approval results
    // as one dim line each, "chat" hides them, "system" shows only them.
    std::string filter = "all";
    // Drawn right after the pane opens (the desktop's CHAT|GRAPH switch lives in Layout).
    std::function<void()> header;
    // The last tab closed: the desktop goes back to the map.
    std::function<void()> onEmpty;
    // What a person's own page links to: the windows it summarises.
    std::function<void()> onApprovals;
    std::function<void()> onNotices;
    // Notices for a person's own page; owned by the runtime.
    void setNotifications(const chat::Notifications* n) { notices_ = n; }
    // Unread by Principal, for the tab badges: owned by the frame.
    const std::map<std::string, int>* unread = nullptr;
    const std::map<std::string, model::Activity>* activity = nullptr; // by agent, for the tabs
    // The Tasks fold (approval cards in the transcript, the PENDING count in the status bar).
    void setTasks(model::Tasks* tasks) { tasks_ = tasks; }
    // The Tokens fold (the DEVICES tab) and where a saved token file goes (the data dir).
    void setTokens(const model::Tokens* tokens) { tokens_ = tokens; }
    void setTokenDir(std::filesystem::path dir) { tokenDir_ = std::move(dir); }
    // The url this listener attached with (the pairing link's default when the world names none).
    void setSocketUrl(std::string url) { socketUrl_ = std::move(url); }
    // A pairing countdown is showing: the app ticks one frame a second while it runs.
    bool countdown() const;
    // The Actions fold (the [ ACTIONS ] menu lists what the world offers).
    void setActions(const model::Actions* actions) { actions_ = actions; }
    // A named action the terminal asked for this frame (the app opens its dialog), with args.
    std::string takeActionRequest() { std::string a; a.swap(actionRequest_); return a; }
    json takeActionArgs() { json a = std::move(actionArgs_); actionArgs_ = json::object(); return a; }
    // Stop the current terminal's running Evaluation: `act {object <evaluation> action cancel}`
    // (Esc twice within 400 ms, Cmd-., or the STOP button). No-op when nothing runs.
    void cancelCurrent(const model::Evaluations& evals, chat::Chat& chat);
    void closeCurrent();
    std::vector<std::string> tabIds() const; // the open tabs, for view/panes
    // A file, folder or text dropped on the window (SDL drop events, or the drive's `drop`): it
    // lands on the current terminal. Files are attached (sprint 1 L8: an Object with the file
    // Fields, then a Message tagging it); a folder asks [ MOUNT ] or [ ATTACH LISTING ]; text
    // becomes a small text attachment.
    void drop(const std::string& path, const std::string& text, ImVec2 at);
    // A file the world can read, for this tab's next line.
    void addImageAt(chat::Chat& chat, const std::string& mount, const std::string& path);
    // A graph view the user asked to EXPAND onto the GRAPH pane: {title, model}, handed over once.
    std::optional<std::pair<std::string, model::GraphModel>> takeGraphOverlay();
    // The current terminal's attach progress (drive `state.attach`).
    struct AttachStatus {
        std::string stage;
        bool err = false;
        bool done = true;
    };
    AttachStatus attachStatus() const;
    // The current tab's view (0 transcript, 1 prompt, 2 fields); the id sweep walks them.
    void setView(int view);
    // /compress on the current tab, as the status bar's button does.
    void compressCurrent(model::Principals& principals, chat::Chat& chat, const Session& session);
    const std::string& current() const;
    // Whose loop the current tab watches (drive `state.evaluation`).
    std::string currentWorker() const;
    bool empty() const { return tabs_.empty(); }

    void draw(model::Principals& principals, const model::Evaluations& evals, chat::Chat& chat, const Session& session,
              const Anim& anim, ImFont* mono);
    void drawBody(model::Principals& principals, const model::Evaluations& evals, chat::Chat& chat,
                  const Session& session, const Anim& anim, ImFont* mono);
    // Expanded views ([ EXPAND ] on a table, a click on an image) as their own windows; the
    // desktop pane calls it itself, the phone frame calls it after drawBody.
    void drawExpandedViews(model::Principals& principals, chat::Chat& chat, ImFont* mono);
    // The same for thread windows; the frames call it right after drawExpandedViews.
    void drawThreadWindows(model::Principals& principals, const model::Evaluations& evals, chat::Chat& chat,
                           const Session& session, ImFont* mono);

    // TOUCH mode: the transcript and its line alone; no tab row, view tabs, filter, status or model bar.
    bool bare = false;
    bool inputActive() const;        // the current tab's line has focus (the cursor blinks)
    // TOUCH mode: the hold-to-talk bar for the current tab; false when no Model can hear.
    bool drawTalkBar(float width, float height);
    bool canTalk() const;
    bool canSpeak() const;
    // TOUCH mode: a tall SPEAK beside the bar: reads the last reply aloud.
    bool drawSpeakBar(float width, float height);
    ImVec2 inputMin() const;         // for SDL_SetTextInputArea on the phone
    ImVec2 inputMax() const;
    void requestFocus();
    // Any open Principal's loop is mid-cycle (the spinner needs frames).
    bool spinning(const model::Principals& principals) const;

  private:
    struct FieldEdit {
        std::string docText; // what the rendered doc last showed
        std::string buffer;
        std::string shown; // read-only copy of the value for the selectable box
    bool entered = false; // Enter in the edit box this frame
        Seq shownFor = -1;
        bool shownLinks = false; // the value names something: drawn linked, not in the box
        Seq basedOn = -1; // the Field seq the buffer was loaded from
        std::string result;
        bool resultErr = false;
        bool editing = false;
        std::string reader = "both"; // a hint's reader (agent | human | both) while editing
        bool focus = false;          // take keyboard focus on the next frame (a hint cell was clicked)
        bool expanded = false;       // FIELDS: a long rendered value shown whole instead of its first lines
        int docTheme = -1;
        // The rendered (markdown) read-only view of a multi-line string value.
        SelDoc doc;
        SelState sel;
        SelLayout layout;
        MdExtras md;
        Seq docFor = -1;
        unsigned long long docSymbols = 0;
        unsigned long long docBuilds = 0;
    };
    struct Head {
        std::string from, message, via, inReplyTo;
        Seq seq;
        std::string contradiction; // reply/contradiction: shown as a red chip, spelled out in the tooltip
    };
    struct Toggle {
        std::string evaluation;
    };
    // An approval card (sprint 1 L5): the paragraphs it spans and the one-line slot its controls
    // ([ APPROVE ] [ DENY ] reason, time left) are drawn into as overlay items.
    struct Card {
        std::string task;
        std::size_t firstPara = 0;
        std::size_t lastPara = 0;
        std::size_t controlsPara = 0;
        int rows = 1; // 2 on a narrow pane: APPROVE / DENY on one row, the reason box and time left on the next
        bool pending = false;
    };
    // A view block (sprint 1 L10): the reserved paragraph a table / tree / graph / image is drawn
    // over; text views go into the document as markdown instead.
    struct ViewBlock {
        std::string view;
        std::size_t para = 0;
        int block = 0;
        std::string via;
    };
    // The transcript as a document, built from the folds only when one of them moved (the
    // folds' applied seqs, the message cache version, the thinking switches); the word-wrap
    // layout of it is cached by width in `layout`. Drawing a frame then costs the visible lines.
    // An object the Principals fold does not carry (a Route, a Task, a Message, a file): read once,
    // and again after a set-field here, for its inspector tab.
    struct ObjectEntry {
        model::PrincipalInfo info;
        bool ready = false;
        std::string err;
    };
    const model::PrincipalInfo* objectInfo(const std::string& id, model::Principals& principals, chat::Chat& chat);
    std::map<std::string, ObjectEntry> objects_;
    std::set<std::string> objectsAsked_;
    std::vector<std::string> pendingNames_;
    SymbolScan scan_;
    const model::Evaluations* fieldsEvals_ = nullptr;
    // [ VIEW PROMPT ]: the exact text of a Principal's latest Serialization, by section.
    struct PromptView {
        std::string principal;
        std::string serialization;
        std::string status;
        std::vector<chat::PromptSection> sections;
    };
    std::optional<PromptView> promptView_;
    void drawPromptView(const model::Principals& principals, ImFont* mono);
    struct View {
        std::vector<chat::TranscriptLine> lines;
        SelDoc doc;
        std::map<std::size_t, Head> heads;
        std::map<std::size_t, Toggle> toggles;
        std::map<std::size_t, json> opTips;
        std::map<std::size_t, std::string> costTips;
        std::vector<Card> cards; // approval cards in paragraph order
        std::vector<ViewBlock> views; // view blocks in paragraph order
        struct ImageRow {
            std::string message;
            std::size_t para = 0;
            nlohmann::json images;
        };
        std::vector<ImageRow> imageRows;
        // A thread (world pass 8): its header paragraph (click collapses), the paragraphs it spans
        // (the rail is drawn down their left edge), and whether it is collapsed to root + last.
        struct Rail {
            std::string thread;
            std::size_t headerPara = 0;
            std::size_t firstPara = 0;
            std::size_t lastPara = 0;
            bool collapsed = false;
            std::size_t linkBegin = 0; // `open thread`, a link run in the header: byte offsets in that paragraph
            std::size_t linkEnd = 0;
        };
        std::vector<Rail> rails;
        std::map<std::size_t, std::string> threadToggles; // header para -> thread id
        // The kernel record box of a loop's reply (world pass 13): the paragraphs it spans (framed,
        // red when the prose contradicted it) and its title paragraph (click folds it).
        struct Record {
            std::string message;
            std::size_t firstPara = 0;
            std::size_t lastPara = 0;
            bool contradiction = false;
        };
        std::vector<Record> records;
        std::map<std::size_t, std::string> recordToggles; // title para -> message id
        float wrap = -1.0f;           // the width the view heights were computed for
        unsigned long long viewsVersion = ~0ULL;
        unsigned long long imagesVersion = ~0ULL;
        MdExtras md;           // code block frames and links of the rendered bodies
        std::size_t shape = 0; // grows when a line lands or fills in (tail following)
        SelLayout layout;
        // key
        Seq principalsAt = -2;
        Seq evalsAt = -2;
        unsigned long long symbolsVersion = 0;
        Seq tasksAt = -2;
        std::string heldCard;
        int themeAt = -1;
        unsigned long long messagesVersion = ~0ULL;
        unsigned long long expandedVersion = ~0ULL;
        bool thinking = false;
        std::string filter;
        std::string self;
        unsigned long long builds = 0;
    };
    // One NEW FIELD row: PROMPT, FIELDS and MEMORY share the widget.
    struct NewField {
        std::string name;
        std::string value;
        int type = 0;   // index into chat::kFieldTypes
        int reader = 2; // agent, human, both
        bool sent = false;
        bool seeded = false;
        bool open = false; // the form shows once its `+` is pressed
    };
    struct Tab {
        std::string principal;
        std::string worker; // whose loop this tab watches
        bool inspector = false; // an inspector tab: an object's Fields, apart from an agent's chat
        bool object = false;    // not a Principal: it has no chat, so its one tab is its inspector
        int selfView = 0;       // a person's own page: you, messages, devices, fields
        int view = 0; // 0 transcript, 1 prompt, 2 fields, 3 memory, 4 devices
        std::string scrollField; // FIELDS scrolls to this row once
        InputPane line;
        std::function<void(const std::string&)> talk; // set while a Model can hear: what the bar hands a recording to
        std::function<void()> speak;                   // set while a Model can speak and there is a reply to read
        // Images waiting for this tab's next send. Shared: an upload outlives a frame.
        struct PendingImage {
            std::string name;
            nlohmann::json attachment; // set once the world holds it
            std::string error;
        };
        std::vector<std::shared_ptr<PendingImage>> images;
        chat::InputLine history;
        std::map<std::string, FieldEdit> edits;
        NewField newPrompt;  // the row under PROMPT, starting at instructions/
        NewField newField;   // the row above the FIELDS table
        NewField newMemory;  // the row in MEMORY, its name under memory/
        std::string forgetting; // the memory Field awaiting the confirm of [ FORGET ]
        std::string clearing;   // the Field awaiting the confirm of [ CLEAR ]
        bool confirmFocus = false; // the frame one of those asks: CANCEL takes the keyboard
        std::string sendError;
        bool stick = true;
        std::string jumpMessage;  // on its way to this Message; given up after a few seconds
        double jumpSince = 0.0;
        std::string flashMessage; // the Message a jump landed on, marked until flashUntil
        double flashUntil = 0.0;
        std::string menuMessage;  // the Message under the last right click
        Seq menuSeq = 0;
        std::size_t lastLines = 0;
        SelState sel;                            // character / block selection in the transcript
        std::vector<std::string> queuePending;   // lines sent while it works, until `loop/queue` lists them
        bool queueInstead = false;
        std::string steerError;                  // why a steer was refused, with the line kept in the input
        std::string steerText;                   // that line, for [ QUEUE INSTEAD ]
        std::vector<std::string> pendingTags;    // picked with `@` for the next line
        std::string lastSent;                    // the last line sent from this tab (RETRY)
        int recipientIndex = 0;                  // which of the Route's other members the line addresses
        int routeIndex = 0;                      // an agent on several Routes: which one this chat uses
        std::set<std::string> expandedThinking;  // evaluation ids whose thinking is open
        std::set<std::string> collapsedThreads;  // thread root ids shown as root + last reply only
        std::set<std::string> collapsedRecords;  // reply Message ids whose kernel record box is folded to its title
        unsigned long long expandedVersion = 0;  // bumped when expandedThinking or collapsedThreads change
        View view_;
        std::string cancelling;                  // the Evaluation (or Principal) a cancel was sent for
        std::string cancelResult;                // the op's err, shown inline
        double escAt = -1.0;                     // steady seconds of the last Escape (Esc-Esc stops)
        std::set<std::string> openThreads;       // Fields whose comment thread is expanded ("object" for the object's)
        std::map<std::string, std::string> replies; // reply box text per thread
        std::map<std::string, std::string> replyResults;
        std::map<std::string, std::string> denyReasons; // per Task: the reason typed before [ DENY ]
        std::map<std::string, std::string> taskResults; // per Task: pending… / ok seq N / err, shown on the card
        std::map<std::string, bool> taskResultErr;
        struct ConversationsUi {
            std::vector<chat::ConversationRow> rows;
            std::string search, err, status;
            bool asked = false, busy = false, more = false, supported = true;
        } conv;
        std::string heldCard; // the card under the mouse: it keeps its height until the mouse leaves
        std::map<std::string, ViewUiState> viewUi;      // per view id: table sort, tree folds, graph pan/zoom, texture
        std::vector<std::pair<std::string, std::string>> pendingDrops; // {path, text} dropped, not yet processed
        std::string folderPrompt;                       // a dropped folder awaiting MOUNT / ATTACH LISTING
        FieldEdit reasoningEdit;                        // the loop/reasoning write from the status bar
        // The Model control: what the world says a wake runs on, asked again only when the fold changes.
        json chosen;
        std::string chosenStamp;
        int chosenAsked = 0;
        std::string lastThread;    // the conversation on screen, for a per-conversation choice
        bool pickerConversation = false;
        int pickerCursor = 0; // the row Up, Down and Return act on
        std::string pickerResult;  // a refusal, shown beside the control
        FieldEdit modelEdit;                           // the principal/models write from the MODEL menu
        std::string modelChoice;                       // the Model the last choice named (shown beside the result)
        // DEVICES: revoke reasons and results per token, the NEW DEVICE form and the one-time secret
        std::map<std::string, std::string> revokeReasons;
        std::map<std::string, std::string> tokenResults;
        std::map<std::string, bool> tokenResultErr;
        bool newDevice = false;
        std::string newDeviceLabel;
        double newDeviceDays = 30;
        std::string issueStatus;
        bool issueErr = false;
        std::string issuedSecret; // shown once, copyable, never logged
        std::string issuedLabel;
        std::string issuedObject;
        bool issuedSaved = false;
        // OPEN THREAD (world pass 8/14): a thread shown whole in its own window, every participant's
        // Messages in log order; filled from `history {thread}` when the world has it, else the fold.
        struct ThreadUi {
            std::string thread;
            bool requested = false;     // history asked once
            std::string source = "loading"; // world | fold | loading
            std::string err;
            std::size_t took = 0;      // events or messages the history answer carried
            int themeAt = -1;
            std::vector<chat::TranscriptLine> lines;
            SelDoc doc;
            SelState sel;
            SelLayout layout;
            MdExtras md;
            std::map<std::size_t, Head> heads;
            std::map<std::size_t, std::string> costTips;
            unsigned long long builds = 0;
            unsigned long long messagesVersion = ~0ULL;
            Seq principalsAt = -2;
            Seq evalsAt = -2;
            float wrap = -1.0f;
        };
        std::map<std::string, ThreadUi> threadUi; // open thread windows by thread root id
        // PAIR PHONE: pair.begin's code and expiry, the link, its QR
        bool pairOpen = false;
        std::string pairStatus;
        bool pairErr = false;
        std::string pairCode;
        std::string pairExpiresAt;
        std::string pairUrl;
        std::string pairLink;
        std::vector<std::uint8_t> pairQr; // size*size modules
        int pairQrSize = 0;
    };

    void rebuildView(Tab& tab, const model::PrincipalInfo* info, model::Principals& principals,
                     const model::Evaluations& evals, chat::Chat& chat, const std::string& self, float wrap);
    // The view block overlays (title bar, table / tree / graph / image bodies) after the document.
    void drawViewBlocks(Tab& tab, model::Principals& principals, chat::Chat& chat, ImVec2 origin, float wrap, ImFont* mono);
    // A ref the user clicked in a view: a Principal opens its FIELDS, anything else is copied.
    void openRef(Tab& tab, model::Principals& principals, const std::string& ref);
    std::string lastJump_; // the last Message a jump reached
    // Dropped files queued on the tab: classify, attach or prompt (folders).
    void processDrops(Tab& tab, chat::Chat& chat, const Session& session, const std::vector<std::string>& routes);
    void cancel(Tab& tab, const model::Evaluations& evals, chat::Chat& chat);
    bool inlineTabs_ = false; // the desktop header (CHAT|GRAPH switch) shares row 1 with the tabs

    void drawTranscript(Tab& tab, const model::PrincipalInfo* info, model::Principals& principals,
                        const model::Evaluations& evals, chat::Chat& chat, const Session& session, const Anim& anim, ImFont* mono);
    void drawPrompt(Tab& tab, const model::PrincipalInfo* info, const model::Principals& principals, chat::Chat& chat);
    void sendDirective(Tab& tab, const model::PrincipalInfo* info, const model::Principals& principals, chat::Chat& chat,
                       const Session& session, const std::string& line);
    void askConversations(Tab& tab, chat::Chat& chat, long long before);
    void drawConversations(Tab& tab, const model::PrincipalInfo* info, const model::Principals& principals, chat::Chat& chat,
                           const Session& session);
    void drawPolicy(Tab& tab, const model::PrincipalInfo* info, const model::Principals& principals, chat::Chat& chat);
    RuleForm policyForm_;
    bool policyAdding_ = false;
    std::string policyStatus_;
    // REASONING OFF | ON and SHOW, beside the input line.
    void addImageBytes(Tab& tab, chat::Chat& chat, std::vector<unsigned char> bytes, const std::string& name);
    void addImageFile(Tab& tab, chat::Chat& chat, const std::string& path);
    void drawPendingImages(Tab& tab);
    void drawModelPicker(Tab& tab, const model::PrincipalInfo* info, const model::Principals& principals, chat::Chat& chat);
    void drawReasoning(Tab& tab, const model::PrincipalInfo* info, const model::Principals& principals, chat::Chat& chat);
    void drawStatusBar(Tab& tab, const model::PrincipalInfo* info, model::Principals& principals,
                       const model::Evaluations& evals, chat::Chat& chat, const Session& session);
    void drawSelfPage(Tab& tab, const model::PrincipalInfo* info, model::Principals& principals, const model::Evaluations& evals,
                      chat::Chat& chat, const Session& session, const Anim& anim, ImFont* mono);
    void drawSelfSummary(Tab& tab, const model::PrincipalInfo* info, model::Principals& principals, chat::Chat& chat,
                         const Session& session);
    void drawMemory(Tab& tab, const model::PrincipalInfo* info, const model::Principals& principals, chat::Chat& chat,
                    ImFont* mono);
    // DEVICES: the Principal's token Objects (label, issued, expires, last used, state) with
    // [ REVOKE ] per row, [ NEW DEVICE… ] -> token.issue whose secret shows once with [ COPY ] and
    // [ SAVE TO FILE ], and [ PAIR PHONE ] -> pair.begin (code, url, QR).
    void drawDevices(Tab& tab, const model::PrincipalInfo* info, const model::Principals& principals, chat::Chat& chat,
                     const Session& session, ImFont* mono);
    // The pairing block under DEVICES: pair.begin, then the code large, the world url, time left
    // and the QR of orbis://pair?url=…&code=… until it expires.
    void drawPairing(Tab& tab, const std::string& who, chat::Chat& chat, const Session& session, ImFont* mono);
    void drawFields(Tab& tab, const model::PrincipalInfo* info, const model::Principals& principals, chat::Chat& chat,
                    const Session& session);
    // The HINT cell of a Field (or of the object when `base` is "object"): the one-line
    // `<base>/hint` value, click to edit, with the reader selector; [ SET ] writes it.
    // `prefix` is fixed in front of the name (memory/); `scope` names the drive targets
    // (new-field: input:new-field:name, input:new-field:value, menu:new-field:type:<t>, button:set-field).
    void drawNewField(Tab& tab, NewField& nf, const model::PrincipalInfo* info, chat::Chat& chat, const char* prefix,
                      const char* scope);
    void drawContextBar(Tab& tab, const model::Evaluations& evals);
    void drawQueue(Tab& tab, const std::vector<chat::QueueRow>& queue, const model::Principals& principals, chat::Chat& chat);
    void drawHintCell(Tab& tab, const model::PrincipalInfo* info, chat::Chat& chat, const std::string& base, float width);
    // The comment thread of a Field or the object: `<base>/comment`, a list of {by at text},
    // each line with the author's name and time, and a reply box that appends and writes back.
    void drawThread(Tab& tab, const model::PrincipalInfo* info, const model::Principals& principals, chat::Chat& chat,
                    const Session& session, const std::string& base);
    void setField(Tab& tab, chat::Chat& chat, const std::string& name, const json& type, const std::string& reader,
                  const json& value, FieldEdit& edit);
    // A string Field rendered as markdown, selectable, with the code frames and link tooltips;
    // used by PROMPT and FIELDS. `key` scopes the ImGui ids.
    // A Field too long for a row: a string with lines, or more than a row can hold. Its EDIT opens the
    // editor (editInPane), its row shows the first lines (foldAt) and how much there is (sizeLine).
    static bool tallField(const model::FieldState& f);
    static std::size_t foldAt(const std::string& whole, float charsPerLine);
    static std::string sizeLine(const std::string& whole);
    void editInPane(const Tab& tab, const std::string& name);
    void clearField(const Tab& tab, chat::Chat& chat, const std::string& name, FieldEdit& edit);
    // `applies at root's next wake`, or `root has seen this`, for an edit of someone else's.
    void drawFieldNews(const model::PrincipalInfo& info, const model::FieldState& f, const model::Principals& principals);
    void drawCompression(Tab& tab, const model::PrincipalInfo& info, chat::Chat& chat);
    void drawRendered(FieldEdit& edit, Seq seq, const std::string& value, float wrap, ImFont* mono, const char* key, const nlohmann::json* refs = nullptr);

    // Approve or deny a Task from its card: `act {action task.approve|task.deny args {task note?|reason}}`.
    void decide(Tab& tab, chat::Chat& chat, const std::string& task, bool approve);
    // The card overlays (frame, controls, result) after the transcript document is drawn.
    void drawCards(Tab& tab, model::Principals& principals, chat::Chat& chat, const Session& session, ImVec2 origin,
                   float wrap);
    // The thread windows ([ OPEN THREAD ] on a rail header): request `history {thread}` once, then
    // render every Message of the thread from the fold with the cost line per reply.
    void drawThreadPanes(Tab& tab, model::Principals& principals, const model::Evaluations& evals, chat::Chat& chat,
                         const Session& session, ImFont* mono);
    void rebuildThreadDoc(Tab& tab, Tab::ThreadUi& tu, model::Principals& principals, const model::Evaluations& evals,
                          chat::Chat& chat, const std::string& self, float wrap);

    std::vector<Tab> tabs_;
    TabBarState tabBarState_;
    int current_ = -1;
    std::map<std::string, AttachStatus> attach_; // per Principal: the last drop's progress (stable addresses)
    std::optional<std::pair<std::string, model::GraphModel>> graphOverlay_;
    bool focusRequested_ = false;
    model::Tasks* tasks_ = nullptr;
    std::set<std::string> taskReads_; // Tasks read once because a card arrived before their Fields
    const chat::Notifications* notices_ = nullptr;
    const model::Tokens* tokens_ = nullptr;
    std::filesystem::path tokenDir_;
    std::string socketUrl_;
    const model::Actions* actions_ = nullptr;
    std::string actionRequest_;
    json actionArgs_ = json::object();
    // MODEL menu: the answer of the last model.probe per Model id (stable addresses, the
    // handler runs after the frame).
    std::map<std::string, std::pair<std::string, bool>> probeResults_;
};

} // namespace ke::ui
