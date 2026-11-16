// Port of the original test/ui.test.ts.

#include "TestUtil.h"

#include "core/Agent.h"
#include "tui/Editor.h"
#include "tui/Keys.h"
#include "tui/Markdown.h"
#include "tui/Picker.h"
#include "tui/Rain.h"
#include "tui/Renderer.h"
#include "tui/Style.h"
#include "tui/Text.h"
#include "frontend/Commands.h"
#include "frontend/Duration.h"
#include "Entries.h"
#include "frontend/Folders.h"
#include "frontend/Greetings.h"
#include "frontend/Replay.h"
#include "TerminalTheme.h"
#include "View.h"

using namespace Smith;
using namespace Smith::Tui;
using namespace TestUtil;

class TestUi : public QObject {
  Q_OBJECT

private Q_SLOTS:
  // text width and sanitizing
  void countsTerminalColumns();
  void stripsEscapesAndControls();
  void themesColorTheTerminal();
  void approvalPromptCantBeSpoofed();

  // wrap and truncate
  void wrapsAtSpaces();
  void wrapKeepsNewlinesAndSplitsLongWords();
  void wrapKeepsStyles();
  void wrapsWideCharacters();
  void truncateCutsWithEllipsis();

  // markdown
  void markdownInlineStyles();
  void markdownBlocks();
  void streamedTextSplitsOutsideCode();

  // keys
  void parsesTypingArrowsModifiers();
  void kittyKeyboardProtocol();
  void altEnterAndCtrlJAreDistinct();
  void bracketedPasteAcrossReads();

  // line editor
  void editsByCharacterAndWord();
  void newLineKeys();
  void cursorStaysVisible();
  void enterSubmitsAndHistoryRecalls();

  // pickers and folders
  void pickerFiltersAndSelects();
  void pickerBackspaceGoesBack();
  void pickerActionAsksFirst();
  void folderListing();
  void boxPadsLines();

  // renderer
  void rendererPrintsAboveLiveRegion();
  void rendererSkipsUnchangedFrame();
  void rendererReset();

  // Agent Smith
  void greetingsFitTheStart();
  void farewellCountsLongRuns();
  void durationsAndSummaries();
  void longPathsKeepTheirEnd();

  // command suggestions
  void slashListsAndNarrows();
  void argumentsComplete();
  void newCompletesFolders();
  void ordinaryMessagesGetNoSuggestions();

  // digital rain
  void everyFrameFillsTheScreen();
  void titleAppearsAndCenters();
  void rainStripIsEndless();
  void codeStreamScrolls();
  void statusLineHasCodeStream();

  // todos and questions
  void checklistShowsProgress();
  void questionBoxNumbersOptions();
  void resumedSessionShowsChecklists();
};

#ifndef Q_MOC_RUN

namespace {

QStringList plain(const QList<Line>& lines) {
  QStringList out;
  for (const Line& line : lines) {
    QString text;
    for (const Span& span : line) {
      text += span.text;
    }
    out << text;
  }
  return out;
}

QString plainLine(const Line& line) {
  return plain({line}).first();
}

Line text(const QString& value, const Style& style = {}) {
  return Line{Span{value, style}};
}

QList<Key> keys(const QString& data) {
  return KeyReader().feed(data);
}

QString describe(const Key& key) {
  static const QStringList names = {"text", "paste", "char", "return", "backspace", "delete", "tab", "escape",
                                    "up",   "down",  "left", "right",  "home",      "end",    "pageup", "pagedown"};
  QStringList parts{names[int(key.name)]};
  if (!key.text.isEmpty()) {
    parts << key.text;
  }
  if (key.ctrl) {
    parts << "ctrl";
  }
  if (key.meta) {
    parts << "meta";
  }
  if (key.shift) {
    parts << "shift";
  }
  return parts.join(' ');
}

QStringList describeAll(const QList<Key>& list) {
  QStringList out;
  for (const Key& key : list) {
    out << describe(key);
  }
  return out;
}

// Feeds `data` to the editor; true if a key submitted.
bool type(LineEditor& editor, const QString& data) {
  for (const Key& key : KeyReader().feed(data)) {
    if (editor.handle(key)) {
      return true;
    }
  }
  return false;
}

Key named(KeyName name) {
  Key key;
  key.name = name;
  return key;
}

Key ctrl(QChar letter) {
  Key key;
  key.name = KeyName::Char;
  key.ctrl = true;
  key.text = QString(letter);
  return key;
}

Key typed(const QString& value) {
  Key key;
  key.name = KeyName::Text;
  key.text = value;
  return key;
}

// The rows of a full-screen frame, without colors.
QStringList frameRows(const QString& frame) {
  QStringList rows = frame.split(QRegularExpression("\\x1b\\[\\d+;1H"));
  rows.removeFirst();
  for (QString& row : rows) {
    row.remove(QRegularExpression("\\x1b\\[[\\d;]*m"));
  }
  return rows;
}

struct RecordingScreen {
  QStringList writes;

  Screen screen() {
    return Screen{[this](const QString& text) { writes << text; }, []() { return 20; }, []() { return 10; }};
  }
};

QString strip(QString text) {
  return text.remove("\x1b[?2026h").remove("\x1b[?2026l");
}

QStringList trimmedText(const QList<Line>& lines) {
  QStringList out;
  for (QString line : plain(lines)) {
    while (line.endsWith(' ')) {
      line.chop(1);
    }
    out << line;
  }
  return out;
}

Ui::Status baseStatus() {
  Ui::Status status;
  status.endpoint = "local";
  status.model = "m";
  status.reasoning = "default";
  status.sandboxActive = true;
  status.sandboxType = SandboxType::Bwrap;
  return status;
}

} // namespace

void TestUi::countsTerminalColumns() {
  QCOMPARE(textWidth("hello"), 5);
  QCOMPARE(textWidth("日本語"), 6);
  QCOMPARE(textWidth("é"), 1);
  QCOMPARE(textWidth("👍🏽"), 2);
  QCOMPARE(textWidth("👨‍👩‍👧"), 2);
  QCOMPARE(textWidth("⚠️"), 2);
}

void TestUi::stripsEscapesAndControls() {
  QCOMPARE(sanitize("ok\x1b[31mred\x1b[0m\x1b]0;title\x07\r\nnext\x08"), QString("okred\nnext"));
  QCOMPARE(sanitize("a\tb"), QString("a    b"));
}

void TestUi::approvalPromptCantBeSpoofed() {
  // Carriage return + erase-line would hide "rm -rf ~" in a real terminal.
  const QString line = toAnsi(text("rm -rf ~\r\x1b[2Kls"));
  QVERIFY(!line.contains('\r') && !line.contains("\x1b[2K"));
  QVERIFY(line.contains("rm -rf ~"));
}

void TestUi::wrapsAtSpaces() {
  QCOMPARE(plain(wrap(text("the quick brown fox jumps"), 10)), QStringList({"the quick", "brown fox", "jumps"}));
}

void TestUi::wrapKeepsNewlinesAndSplitsLongWords() {
  QCOMPARE(plain(wrap(text("a\n  indented\nabcdefghij"), 4)), QStringList({"a", "  in", "dent", "ed", "abcd", "efgh", "ij"}));
}

void TestUi::wrapKeepsStyles() {
  const QList<Line> lines = wrap(Line{Span{"plain ", {}}, Span{"bold words here", Style{.bold = true}}}, 11);
  QCOMPARE(plain(lines), QStringList({"plain bold", "words here"}));
  QVERIFY(lines[1][0].style.bold);
}

void TestUi::wrapsWideCharacters() {
  QCOMPARE(plain(wrap(text("日本語のテキスト"), 6)), QStringList({"日本語", "のテキ", "スト"}));
}

void TestUi::truncateCutsWithEllipsis() {
  QCOMPARE(plainLine(truncate(text("abcdef"), 4)), QString("abc…"));
  QCOMPARE(plainLine(truncate(text("abc"), 4)), QString("abc"));
  QCOMPARE(lineWidth(truncate(text("日本語のテキスト"), 5)), 5);
}

void TestUi::markdownInlineStyles() {
  const Line spans = inlineSpans("use `npm test`, **bold**, *it* and snake_case_name");
  QStringList shown;
  for (const Span& span : spans) {
    const QString kind = span.style.color == Color::Cyan ? "cyan" : span.style.bold ? "bold" : span.style.italic ? "italic" : "";
    shown << span.text + "|" + kind;
  }
  QCOMPARE(shown, QStringList({"use |", "npm test|cyan", ", |", "bold|bold", ", |", "it|italic", " and snake_case_name|"}));
}

void TestUi::markdownBlocks() {
  const QString markdown = "# Title\n\n- one\n- two that wraps\n\n```ts\nconst x = 1;\n```\n1. first";
  QCOMPARE(plain(markdownLines(markdown, 14)), QStringList({"Title", "", "• one", "• two that", "  wraps", "", "│ const x = 1;", "1. first"}));
}

void TestUi::streamedTextSplitsOutsideCode() {
  QCOMPARE(safeSplit("para one\n\npara two"), 8);
  QCOMPARE(safeSplit("intro\n\n```\ncode\n\nmore code"), 5);
  QCOMPARE(safeSplit("```\na\n\nb"), -1);
}

void TestUi::parsesTypingArrowsModifiers() {
  QCOMPARE(describeAll(keys("ab\x1b[A\x1b[1;5C\x7f\r\x03\x1b" "b\x1b")),
           QStringList({"text ab", "up", "right ctrl", "backspace", "return", "char c ctrl", "char b meta", "escape"}));
}

void TestUi::kittyKeyboardProtocol() {
  QCOMPARE(describeAll(keys("\x1b[13;2u\x1b[27u\x1b[99;5u\x1b[97;3u\x1b[13u")),
           QStringList({"return shift", "escape", "char c ctrl", "char a meta", "return"}));
}

void TestUi::altEnterAndCtrlJAreDistinct() {
  QCOMPARE(describeAll(keys("\x1b\r\n\r")), QStringList({"return meta", "return ctrl", "return"}));
}

void TestUi::bracketedPasteAcrossReads() {
  KeyReader reader;
  QVERIFY(reader.feed("\x1b[200~line one\nline").isEmpty());
  QCOMPARE(describeAll(reader.feed(" two\x1b[201~x")), QStringList({"paste line one\nline two", "text x"}));
}

void TestUi::editsByCharacterAndWord() {
  LineEditor editor;
  type(editor, "hello wörld");
  // left, left, backspace
  type(editor, "\x1b[D\x1b[D\x7f");
  QCOMPARE(editor.value(), QString("hello wöld"));
  // ctrl+w
  type(editor, "\x17");
  QCOMPARE(editor.value(), QString("hello ld"));
  // end, ctrl+a, X
  type(editor, "\x1b[F\x01X");
  QCOMPARE(editor.value(), QString("Xhello ld"));
}

void TestUi::newLineKeys() {
  LineEditor editor;
  QCOMPARE(type(editor, "one\x1b[13;2utwo\x1b\rthree\nfour\\\rfive"), false);
  QCOMPARE(editor.value(), QString("one\ntwo\nthree\nfour\nfive"));
  QVERIFY2(type(editor, "\r"), "plain enter still submits");
}

void TestUi::cursorStaysVisible() {
  LineEditor editor;
  type(editor, "abc");
  QCOMPARE(plain(wrap(editor.render("placeholder"), 10)), QStringList({"abc "}));
  // When the text fills the line exactly, the cursor moves to the next one rather than vanishing.
  const QList<Line> full = wrap(editor.render("placeholder"), 3);
  QCOMPARE(plain(full), QStringList({"abc", " "}));
  bool inverse = false;
  for (const Span& span : full[1]) {
    inverse = inverse || span.style.inverse;
  }
  QVERIFY(inverse);
}

void TestUi::enterSubmitsAndHistoryRecalls() {
  LineEditor editor;
  QVERIFY(type(editor, "first\r"));
  QCOMPARE(editor.commit(), QString("first"));
  type(editor, "second");
  editor.commit();
  type(editor, "draft\x1b[A");
  QCOMPARE(editor.value(), QString("second"));
  type(editor, "\x1b[A");
  QCOMPARE(editor.value(), QString("first"));
  type(editor, "\x1b[B\x1b[B");
  QCOMPARE(editor.value(), QString("draft"));
}

void TestUi::pickerFiltersAndSelects() {
  Picker picker("Pick", {{"a", "qwen3-coder", {}}, {"b", "gpt-oss-20b", {}}, {"c", "qwen3-small", {}}});
  for (const Key& key : keys("qwen\x1b[B")) {
    picker.handle(key);
  }
  QCOMPARE(picker.handle(named(KeyName::Return)), Picker::Outcome::Chose);
  QCOMPARE(picker.chosen().value, QString("c"));
  QVERIFY(!picker.chosen().action);
  QCOMPARE(picker.handle(named(KeyName::Escape)), Picker::Outcome::Cancel);
}

void TestUi::pickerBackspaceGoesBack() {
  PickerOptions options;
  options.back = "up";
  Picker picker("Folder", {{"a", "alpha/", {}}}, {}, options);
  for (const Key& key : keys("al")) {
    picker.handle(key);
  }
  QVERIFY2(picker.handle(named(KeyName::Backspace)) == Picker::Outcome::Continue, "first it edits the filter");
  picker.handle(named(KeyName::Backspace));
  QCOMPARE(picker.handle(named(KeyName::Backspace)), Picker::Outcome::Chose);
  QCOMPARE(picker.chosen().value, QString("up"));
}

void TestUi::pickerActionAsksFirst() {
  PickerOptions options;
  options.action = PickerAction{'d', "delete", [](const PickerItem& item) {
                                  return item.value == "cur" ? PickerAction::Check{{}, "Not this one."}
                                                             : PickerAction::Check{QString("Delete %1?").arg(item.label), {}};
                                }};
  Picker picker("Sessions", {{"cur", "current one", {}}, {"old", "old one", {}}}, "cur", options);
  auto second = [&]() { return plain(picker.lines())[1]; };
  QVERIFY_MATCH(second(), "ctrl\\+d delete");

  QCOMPARE(picker.handle(ctrl('d')), Picker::Outcome::Continue);
  QVERIFY2(second() == "Not this one.", "refused for the current item");
  picker.handle(named(KeyName::Down));
  picker.handle(ctrl('d'));
  QCOMPARE(second(), QString("Delete old one? y/n"));
  QVERIFY2(picker.handle(typed("n")) == Picker::Outcome::Continue, "n backs out");
  QVERIFY_MATCH(second(), "type to filter");
  picker.handle(ctrl('d'));
  QCOMPARE(picker.handle(typed("y")), Picker::Outcome::Chose);
  QCOMPARE(picker.chosen().value, QString("old"));
  QVERIFY(picker.chosen().action);
}

void TestUi::folderListing() {
  TempDir dir(QDir::tempPath(), "agent-folders-");
  for (const QString folder : {"beta", "Alpha", ".hidden", "repo/.git"}) {
    QDir().mkpath(dir.file(folder));
  }
  write(dir.file("file.txt"), "");
  QVERIFY(QFile::link(dir.file("beta"), dir.file("link")));
  QCOMPARE(listFolders(dir.path()).names, QStringList({"Alpha", "beta", "link", "repo"}));
  QVERIFY(isGitRepo(dir.file("repo")));
  QVERIFY(!isGitRepo(dir.file("beta")));
  QVERIFY(QRegularExpression("ENOENT|no such", QRegularExpression::CaseInsensitiveOption).match(listFolders(dir.file("missing")).error).hasMatch());
}

void TestUi::boxPadsLines() {
  QCOMPARE(plain(Ui::box({text("hi"), text("longer")}, Color::Cyan, 40)), QStringList({"╭────────╮", "│ hi     │", "│ longer │", "╰────────╯"}));
}

void TestUi::themesColorTheTerminal() {
  const Line warning{Span{"careful", fg(Color::Yellow)}};
  const Line plainText{Span{"text", {}}};
  setPalette(Ui::terminalPalette(Theme::Light));

  // 24-bit: the theme's own shade, here a yellow that reads on white.
  setColorDepth(ColorDepth::TrueColor);
  QCOMPARE(toAnsi(warning), QString("\x1b[38;2;154;103;0mcareful\x1b[0m"));
  // Light and plain leave text without a color to the terminal; matrix tints it.
  QCOMPARE(toAnsi(plainText), QString("text"));
  setPalette(Ui::terminalPalette(Theme::Matrix));
  QCOMPARE(toAnsi(plainText), QString("\x1b[38;2;198;242;208mtext\x1b[0m"));

  // 256 colors: the nearest of them.
  setColorDepth(ColorDepth::Indexed256);
  QCOMPARE(nearestIndexed(0xff0000), 196);
  QCOMPARE(nearestIndexed(0x808080), 244);
  QCOMPARE(toAnsi(warning), QString("\x1b[38;5;%1mcareful\x1b[0m").arg(nearestIndexed(0xe8c46a)));

  // 16 colors, or none: as before themes.
  setColorDepth(ColorDepth::Basic);
  QCOMPARE(toAnsi(warning), QString("\x1b[33mcareful\x1b[0m"));
  QCOMPARE(toAnsi(plainText), QString("text"));
  setColorDepth(ColorDepth::TrueColor);
  setColors(false);
  QCOMPARE(toAnsi(warning), QString("careful"));
  setColors(true);

  setPalette({});
  setColorDepth(detectColorDepth());
}

void TestUi::rendererPrintsAboveLiveRegion() {
  setColors(false);
  RecordingScreen s;
  InlineRenderer renderer(s.screen());
  renderer.draw({text("live 1"), text("live 2")});
  QCOMPARE(strip(s.writes[0]), QString("live 1\r\nlive 2"));
  renderer.print({text("done")});
  renderer.draw({text("input")});
  // Back to column 0, up one row (two live rows), clear down, then output.
  QCOMPARE(strip(s.writes[1]), QString("\r\x1b[1A\x1b[Jdone\r\ninput"));
  setColors(true);
}

void TestUi::rendererSkipsUnchangedFrame() {
  RecordingScreen s;
  InlineRenderer renderer(s.screen());
  renderer.draw({text("same")});
  renderer.draw({text("same")});
  QCOMPARE(s.writes.size(), 1);
  renderer.print({text("new")});
  renderer.draw({text("same")});
  QVERIFY2(s.writes.size() == 2, "queued output still gets printed");
}

void TestUi::rendererReset() {
  RecordingScreen s;
  InlineRenderer renderer(s.screen());
  renderer.draw({text("a"), text("b")});
  renderer.reset();
  renderer.draw({text("c")});
  QCOMPARE(s.writes[1], QString("\x1b[H\x1b[2J\x1b[3J"));
  QVERIFY2(strip(s.writes[2]) == "c", "nothing left to erase after a reset");
}

void TestUi::greetingsFitTheStart() {
  const QDateTime now = QDateTime::fromString("2026-11-07T12:00:00Z", Qt::ISODate);
  auto ago = [&](qint64 ms) { return now.addMSecs(-ms); };
  auto make = [](bool resumed, bool newFolder, std::optional<QDateTime> lastUsed) { return GreetingContext{resumed, newFolder, lastUsed}; };
  for (double r : {0.0, 0.5, 0.999}) {
    for (const GreetingContext& context : {make(false, false, {}), make(false, true, {}), make(true, false, {}), make(true, false, ago(60000)),
                                               make(true, false, ago(40LL * 86400000))}) {
      QVERIFY_MATCH(greeting(context, [r]() { return r; }, now), "Mr\\. Anderson");
    }
  }
  auto zero = []() { return 0.0; };
  QCOMPARE(greeting(make(true, false, {}), zero, now), QString("We meet again, Mr. Anderson."));
  QCOMPARE(greeting(make(true, false, ago(3 * 3600000)), zero, now), QString("We meet again, Mr. Anderson."));
  QCOMPARE(greeting(make(true, false, ago(5 * 60000)), zero, now), QString("Back so soon, Mr. Anderson?"));
  QVERIFY_MATCH(greeting(make(true, false, ago(30LL * 86400000)), zero, now), "long time");
  QVERIFY_MATCH(greeting(make(false, true, {}), zero, now), "new place");
}

void TestUi::farewellCountsLongRuns() {
  QCOMPARE(farewell(20 * 60000), QString("Goodbye, Mr. Anderson."));
  QCOMPARE(farewell(70 * 60000), QString("An hour, Mr. Anderson. It is inevitable. Goodbye."));
  QCOMPARE(farewell(qint64(3.5 * 3600000)), QString("Three hours, Mr. Anderson. It is inevitable. Goodbye."));
  QCOMPARE(farewell(30LL * 3600000), QString("30 hours, Mr. Anderson. It is inevitable. Goodbye."));
}

void TestUi::durationsAndSummaries() {
  QStringList formatted;
  for (qint64 ms : {38LL, 4200LL, 42900LL, 65000LL, 3725000LL}) {
    formatted << formatDuration(ms);
  }
  QCOMPARE(formatted, QStringList({"38ms", "4.2s", "42s", "1m 05s", "1h 02m"}));
  QCOMPARE(turnSummary("stop", 83000, 7), QString("Done in 1m 23s · 7 tool calls"));
  QCOMPARE(turnSummary("stop", 2500, 1), QString("Done in 2.5s · 1 tool call"));
  QCOMPARE(turnSummary("stop", 2500, 0), QString("Done in 2.5s"));
  QCOMPARE(turnSummary("interrupted", 12000, 2), QString("Interrupted after 12s · 2 tool calls"));
  QCOMPARE(turnSummary("max_steps", 600000, 50), QString("Stopped at the step limit after 10m 00s · 50 tool calls"));
  QCOMPARE(turnSummary({}, 3000, 0), QString("Stopped after 3.0s"));
}

void TestUi::longPathsKeepTheirEnd() {
  QCOMPARE(shortPath("/srv/projects/work/webapp", 40), QString("/srv/projects/work/webapp"));
  QCOMPARE(shortPath("/very/long/path/to/some/deep/folder/work/webapp", 20), QString("…/folder/work/webapp"));
  QCOMPARE(shortPath(QDir::homePath() + "/git/agent", 40), QString("~/git/agent"));
  QVERIFY2(shortPath("/x/an-extremely-long-folder-name-that-does-not-fit", 12) == "…oes-not-fit", "a single name too long to fit keeps its end");
}

namespace {

CompletionContext completion(const QString& cwd = QDir::tempPath()) {
  return CompletionContext{cwd, {"local", "gpu-box"}, {"qwen3-coder-30b", "gpt-oss-20b"}, {"fix-login", "add-export"}};
}

QStringList texts(const QString& input, const CompletionContext& ctx = completion()) {
  QStringList out;
  for (const Suggestion& suggestion : suggest(input, ctx)) {
    out << suggestion.text;
  }
  return out;
}

} // namespace

void TestUi::slashListsAndNarrows() {
  QCOMPARE(texts("/").size(), commands().size());
  QCOMPARE(texts("/hel"), QStringList({"/help"}));
  QCOMPARE(texts("/h").first(), QString("/help"));
  QCOMPARE(texts("/n").mid(0, 3), QStringList({"/name", "/new", "/network"}));
  QCOMPARE(texts("/net").first(), QString("/network"));
  QCOMPARE(texts("/xyz"), QStringList());
}

void TestUi::argumentsComplete() {
  QCOMPARE(texts("/reasoning "), QStringList({"/reasoning default", "/reasoning off", "/reasoning low", "/reasoning medium", "/reasoning high"}));
  QCOMPARE(texts("/reasoning h"), QStringList({"/reasoning high"}));
  QCOMPARE(texts("/network o"), QStringList({"/network on", "/network off"}));
  QCOMPARE(texts("/theme "), QStringList({"/theme matrix", "/theme plain", "/theme light"}));
  QCOMPARE(texts("/theme l"), QStringList({"/theme light"}));
  QCOMPARE(texts("/sessions "), QStringList({"/sessions all"}));
  QCOMPARE(texts("/endpoint g"), QStringList({"/endpoint gpu-box"}));
  QCOMPARE(texts("/model oss"), QStringList({"/model gpt-oss-20b"}));
  QVERIFY2(texts("/help ").isEmpty(), "no arguments to complete");
  QCOMPARE(texts("/land "), QStringList({"/land fix-login", "/land add-export"}));
  QCOMPARE(texts("/drop a"), QStringList({"/drop add-export"}));
  QCOMPARE(texts("/worktree f"), QStringList({"/worktree fix-login"}));
  QCOMPARE(texts("/worktree"), QStringList({"/worktree", "/worktrees"}));
}

void TestUi::newCompletesFolders() {
  TempDir dir(QDir::tempPath(), "agent-complete-");
  for (const QString folder : {"webapp/src", "web-old", "billing", ".hidden"}) {
    QDir().mkpath(dir.file(folder));
  }
  const CompletionContext at = completion(dir.path());
  QCOMPARE(texts("/new we", at), QStringList({"/new web-old/", "/new webapp/"}));
  QCOMPARE(texts("/new webapp/", at), QStringList({"/new webapp/src/"}));
  QVERIFY2(texts("/new .h", at) == QStringList({"/new .hidden/"}), "hidden folders when the name starts with a dot");
  QVERIFY(suggest("/new we", at).first().takesArgument);
  for (const QString& suggestion : texts("/new ~/", at)) {
    QVERIFY(suggestion.startsWith("/new ~/"));
  }
}

void TestUi::ordinaryMessagesGetNoSuggestions() {
  QVERIFY(texts("fix the bug").isEmpty());
  QVERIFY(texts("/help\nmore").isEmpty());
  for (const CommandSpec& command : commands()) {
    QVERIFY(commandHelp().contains(command.name));
  }
}

void TestUi::everyFrameFillsTheScreen() {
  Rain::Options options;
  options.title = "AGENT SMITH";
  options.seed = 1;
  Rain rain(60, 20, options);
  while (!rain.done()) {
    rain.step();
    const QStringList rows = frameRows(rain.render());
    QCOMPARE(rows.size(), 20);
    for (const QString& row : rows) {
      QCOMPARE(textWidth(row), 60);
    }
  }
}

void TestUi::titleAppearsAndCenters() {
  Rain::Options options;
  options.title = "AGENT SMITH";
  options.frames = 30;
  options.holdFrames = 5;
  options.seed = 7;
  Rain rain(61, 21, options);
  QCOMPARE(rain.shownTitle().trimmed(), QString());
  for (int i = 0; i < 30; i++) {
    rain.step();
  }
  QCOMPARE(rain.shownTitle(), QString("AGENT SMITH"));
  QVERIFY2(!rain.done(), "the finished title is held for a moment");
  for (int i = 0; i < 5; i++) {
    rain.step();
  }
  QVERIFY(rain.done());
  QCOMPARE(frameRows(rain.render())[10].indexOf("AGENT SMITH"), 25);
}

void TestUi::rainStripIsEndless() {
  Rain::Options options;
  options.frames = std::numeric_limits<double>::infinity();
  Rain strip(22, 3, options);
  int lit = 0;
  for (int i = 0; i < 300; i++) {
    strip.step();
    const QList<Line> lines = strip.lines();
    QCOMPARE(lines.size(), 3);
    for (const Line& line : lines) {
      QCOMPARE(lineWidth(line), 22);
      for (const Span& span : line) {
        lit += span.style == Style{} ? 0 : 1;
      }
    }
  }
  QVERIFY2(!strip.done(), "an endless rain never finishes");
  QVERIFY2(lit > 0, "it rains");
}

void TestUi::codeStreamScrolls() {
  const QString now = codeStream(41, 20);
  QCOMPARE(textWidth(now), 20);
  QCOMPARE(codeStream(42, 19), now.mid(1));
  QVERIFY2(now.contains(' '), "it has gaps");
}

void TestUi::statusLineHasCodeStream() {
  const Ui::Status status = baseStatus();
  Ui::Status streamed = status;
  streamed.filler = [](int n) { return QString(n, QChar(0xFF71)); };
  const Line withStream = Ui::statusLine(streamed, 80);
  QCOMPARE(lineWidth(withStream), 80);
  QVERIFY(plainLine(withStream).endsWith(QString(24, QChar(0xFF71))));
  QVERIFY2(plainLine(Ui::statusLine(streamed, 40)) == plainLine(Ui::statusLine(status, 40)), "no room, no stream");
  QVERIFY(lineWidth(Ui::statusLine(status, 80)) < 80);

  // In a task worktree, its branch comes first, so it's never cut off.
  Ui::Status inWorktree = status;
  inWorktree.worktreeBranch = "smith/fix-login";
  QVERIFY(plainLine(Ui::statusLine(inWorktree, 30)).startsWith("⎇ smith/fix-login · "));
  QVERIFY(!plainLine(Ui::statusLine(status, 80)).contains("⎇"));
}

void TestUi::checklistShowsProgress() {
  const TodoList todos = {{"Read the code", TodoStatus::Completed}, {"Fix the bug", TodoStatus::InProgress}, {"Run the tests", TodoStatus::Pending}};
  QCOMPARE(trimmedText(Ui::todoLines(todos, 40)), QStringList({"● Todos 1/3 done", "  ✔ Read the code", "  ▶ Fix the bug", "  ○ Run the tests"}));
  QCOMPARE(trimmedText(Ui::todoLines({}, 40)), QStringList({"● Todos cleared"}));
}

void TestUi::questionBoxNumbersOptions() {
  const QStringList lines = trimmedText(Ui::questionBox(Question{"Which database?", {"Postgres", "SQLite"}}, 1, 60));
  QVERIFY(lines.contains("│ Which database?                                          │"));
  auto any = [&](const std::function<bool(const QString&)>& test) { return std::any_of(lines.begin(), lines.end(), test); };
  QVERIFY(any([](const QString& line) { return line.startsWith("│   1. Postgres"); }));
  QVERIFY(any([](const QString& line) { return line.startsWith("│ › 2. SQLite"); }));
  QVERIFY(any([](const QString& line) { return line.contains("or 1-2, picks"); }));
  const QStringList open = trimmedText(Ui::questionBox(Question{"Anything else?", {}}, 0, 60));
  QVERIFY(std::any_of(open.begin(), open.end(), [](const QString& line) { return line.contains("type your answer below"); }));
}

void TestUi::resumedSessionShowsChecklists() {
  const QJsonArray todos{QJsonObject{{"content", "Ship it"}, {"status", "pending"}}};
  const QJsonArray calls{QJsonObject{
      {"id", "t1"},
      {"type", "function"},
      {"function", QJsonObject{{"name", "todo_write"}, {"arguments", QString::fromUtf8(QJsonDocument(QJsonObject{{"todos", todos}}).toJson(QJsonDocument::Compact))}}}}};
  const QList<Entry> entries = historyToEntries(
      {
          userMessage("go"),
          Message{{"role", "assistant"}, {"content", QJsonValue::Null}, {"tool_calls", calls}},
          toolMessage("t1", "Todo list updated"),
          userMessage(TODO_REMINDER + "\n[ ] Ship it"),
      },
      {});
  QList<Entry::Kind> kinds;
  for (const Entry& entry : entries) {
    kinds << entry.kind;
  }
  QCOMPARE(kinds, QList<Entry::Kind>({Entry::Kind::User, Entry::Kind::Todos, Entry::Kind::Notice}));
}

#endif

QTEST_GUILESS_MAIN(TestUi)
#include "test_ui.moc"
