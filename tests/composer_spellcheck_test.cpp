#include "composer_spellcheck.h"
#include "ui/UiTestFonts.h"

#include "base/basic_types.h"
#include "base/integration.h"
#include "styles/palette.h"
#include "styles/style_widgets.h"
#include "ui/effects/animations.h"
#include "ui/emoji_config.h"
#include "ui/integration.h"
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include "ui/widgets/fields/input_field.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#include "ui/widgets/popup_menu.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QPointer>
#include <QtCore/QSet>
#include <QtGui/QAction>
#include <QtGui/QContextMenuEvent>
#include <QtGui/QTextCursor>
#include <QtGui/QTextDocument>
#include <QtWidgets/QApplication>
#include <QtWidgets/QMenu>
#include <QtWidgets/QTextEdit>
#include <QtWidgets/QWidget>

#include <algorithm>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

using lingtai::desktop::ComposerSpellCompletion;
using lingtai::desktop::ComposerSpellDocumentTag;
using lingtai::desktop::ComposerSpellPlatform;
using lingtai::desktop::ComposerSpellResult;
using lingtai::desktop::ComposerSpellService;
using lingtai::desktop::ComposerSpellWord;

class TestBaseIntegration final : public base::Integration {
public:
    TestBaseIntegration()
        : base::Integration(0, nullptr) {
        base::Integration::Set(this);
    }

    void enterFromEventLoop(FnMut<void()> &&method) override {
        std::move(method)();
    }
    bool logSkipDebug() override { return true; }
    void logMessageDebug(const QString &) override {}
    void logMessage(const QString &) override {}
};

class TestUiIntegration final : public Ui::Integration {
public:
    TestUiIntegration() {
        Ui::Integration::Set(this);
    }

    void postponeCall(FnMut<void()> &&callable) override {
        std::move(callable)();
    }
    void registerLeaveSubscription(not_null<QWidget *>) override {}
    void unregisterLeaveSubscription(not_null<QWidget *>) override {}
    QString emojiCacheFolder() override { return QString(); }
    QString openglCheckFilePath() override { return QString(); }
    QString angleBackendFilePath() override { return QString(); }
    void touchCounterIncrement() override {}
    int touchCounterNow() override { return 0; }
};

class TestUiRuntime final {
public:
    TestUiRuntime() {
        style::internal::init_palette(style::kScaleDefault);
        style::internal::init_style_widgets(style::kScaleDefault);
        Ui::Emoji::Init();
    }

    ~TestUiRuntime() {
        Ui::Emoji::Clear();
    }

private:
    TestBaseIntegration base_;
    TestUiIntegration ui_;
    Ui::Animations::Manager animations_;
};

void require(bool condition, const char *message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

struct FakeSpellState {
    bool asynchronous = false;
    ComposerSpellDocumentTag next_tag = 40;
    std::vector<ComposerSpellDocumentTag> opened;
    std::vector<ComposerSpellDocumentTag> closed;
    std::vector<std::pair<ComposerSpellDocumentTag, QString>> checked;
    std::vector<QString> learned_calls;
    std::vector<QString> unlearned_calls;
    std::vector<std::pair<ComposerSpellDocumentTag, QString>> ignored_calls;
    QSet<QString> learned;
    std::map<ComposerSpellDocumentTag, QSet<QString>> ignored;
    std::map<QString, ComposerSpellResult> results;
    ComposerSpellCompletion pending;
    ComposerSpellResult pending_result;
};

class FakeSpellPlatform final : public ComposerSpellPlatform {
public:
    explicit FakeSpellPlatform(std::shared_ptr<FakeSpellState> state)
        : state_(std::move(state)) {
    }

    ComposerSpellDocumentTag open_document() override {
        const auto tag = ++state_->next_tag;
        state_->opened.push_back(tag);
        return tag;
    }

    void close_document(ComposerSpellDocumentTag tag) override {
        state_->closed.push_back(tag);
    }

    bool checks_asynchronously() const noexcept override {
        return state_->asynchronous;
    }

    void check(
            ComposerSpellDocumentTag tag,
            const QString &word,
            ComposerSpellCompletion completion) override {
        state_->checked.emplace_back(tag, word);
        auto result = state_->results.contains(word)
            ? state_->results.at(word) : ComposerSpellResult{};
        if (state_->learned.contains(word)) {
            result = ComposerSpellResult{.learned = true};
        } else if (state_->ignored[tag].contains(word)) {
            result = ComposerSpellResult{};
        }
        if (state_->asynchronous) {
            state_->pending = std::move(completion);
            state_->pending_result = std::move(result);
        } else {
            completion(std::move(result));
        }
    }

    void learn(const QString &word) override {
        state_->learned_calls.push_back(word);
        state_->learned.insert(word);
    }

    void unlearn(const QString &word) override {
        state_->unlearned_calls.push_back(word);
        state_->learned.remove(word);
    }

    void ignore(
            ComposerSpellDocumentTag tag,
            const QString &word) override {
        state_->ignored_calls.emplace_back(tag, word);
        state_->ignored[tag].insert(word);
    }

private:
    std::shared_ptr<FakeSpellState> state_;
};

std::shared_ptr<ComposerSpellService> make_service(
        const std::shared_ptr<FakeSpellState> &state) {
    return std::make_shared<ComposerSpellService>(
        std::make_shared<FakeSpellPlatform>(state));
}

Ui::PopupMenu *current_popup(Ui::InputField &field) {
    Ui::PopupMenu *result = nullptr;
    for (auto *widget : field.findChildren<QWidget *>()) {
        if (auto *popup = dynamic_cast<Ui::PopupMenu *>(widget)) {
            result = popup;
        }
    }
    return result;
}

Ui::PopupMenu *request_menu(Ui::InputField &field, int document_position) {
    auto cursor = field.textCursor();
    cursor.setPosition(document_position);
    auto *editor = field.rawTextEdit().get();
    const auto viewport_local = editor->cursorRect(cursor).center();
    const auto editor_local = editor->viewport()->mapTo(
        editor, viewport_local);
    auto event = QContextMenuEvent(
        QContextMenuEvent::Mouse,
        editor_local,
        editor->viewport()->mapToGlobal(viewport_local));
    QApplication::sendEvent(editor, &event);
    QCoreApplication::processEvents();
    return current_popup(field);
}

QAction *named_action(Ui::PopupMenu *popup, const char *name) {
    if (!popup) return nullptr;
    for (const auto action : popup->actions()) {
        if (action->objectName() == QString::fromLatin1(name)) {
            return action;
        }
    }
    return nullptr;
}

QString normalized_action_text(QAction *action) {
    auto result = action->text();
    result.remove('&');
    return result;
}

struct StandardActionState {
    QString text;
    bool enabled = false;
    QKeySequence shortcut;

    bool operator==(const StandardActionState &) const = default;
};

std::vector<StandardActionState> standard_actions(const QList<QAction *> &actions) {
    static const auto standard_labels = QSet<QString>{
        QStringLiteral("Undo"), QStringLiteral("Redo"),
        QStringLiteral("Cut"), QStringLiteral("Copy"),
        QStringLiteral("Copy Link Location"), QStringLiteral("Paste"),
        QStringLiteral("Delete"), QStringLiteral("Select All"),
    };
    auto result = std::vector<StandardActionState>();
    for (auto *action : actions) {
        const auto text = normalized_action_text(action);
        if (standard_labels.contains(text)) {
            result.push_back({text, action->isEnabled(), action->shortcut()});
        }
    }
    return result;
}

std::vector<StandardActionState> standard_actions(Ui::PopupMenu *popup) {
    auto actions = QList<QAction *>();
    for (const auto action : popup->actions()) {
        actions.push_back(action);
    }
    return standard_actions(actions);
}

void verify_word_ranges() {
    QTextDocument document;
    document.setPlainText(QStringLiteral(
        "alpha, café\nnaïve speling 𐐀word 🙂"));

    const auto alpha = lingtai::desktop::composer_spell_word_at(&document, 0);
    require(alpha && alpha->text == QStringLiteral("alpha")
            && alpha->from == 0 && alpha->till == 5,
        "a point inside a word must resolve its exact Qt range");
    const auto adjacent = lingtai::desktop::composer_spell_word_at(&document, 5);
    require(adjacent && adjacent->text == QStringLiteral("alpha"),
        "punctuation immediately after a word must resolve the adjacent word");
    const auto unicode = lingtai::desktop::composer_spell_word_at(&document, 7);
    require(unicode && unicode->text == QStringLiteral("café"),
        "Unicode letters must retain UTF-16 document positions");
    const auto multiline = lingtai::desktop::composer_spell_word_at(
        &document, document.toPlainText().indexOf(QStringLiteral("speling")));
    require(multiline && multiline->text == QStringLiteral("speling"),
        "word lookup must cross QTextDocument paragraph boundaries correctly");
    const auto non_bmp = lingtai::desktop::composer_spell_word_at(
        &document, document.toPlainText().indexOf(QStringLiteral("𐐀")));
    require(non_bmp && non_bmp->text.contains(QStringLiteral("𐐀")),
        "non-BMP Unicode letters must count as word characters");

    QTextDocument punctuation(QStringLiteral("🙂 ! …"));
    require(!lingtai::desktop::composer_spell_word_at(&punctuation, 0),
        "emoji and punctuation without a word must not start a spell request");
}

void verify_replacement_mapping() {
    QWidget host;
    Ui::InputField field(
        &host, st::defaultInputField, Ui::InputField::Mode::MultiLine,
        rpl::single(QStringLiteral("Message")));
    field.setText(QStringLiteral("one speling two"));
    const auto word = ComposerSpellWord{
        .text = QStringLiteral("speling"), .from = 4, .till = 11};

    auto selection = field.textCursor();
    selection.setPosition(0);
    selection.setPosition(3, QTextCursor::KeepAnchor);
    field.setTextCursor(selection);
    require(lingtai::desktop::replace_composer_spell_word(
            &field, word, QStringLiteral("spelling")),
        "a current exact range must accept a suggestion");
    require(field.getLastText() == QStringLiteral("one spelling two")
            && field.textCursor().selectionStart() == 0
            && field.textCursor().selectionEnd() == 3,
        "replacement must preserve a selection before the corrected word");

    field.setText(QStringLiteral("one speling two"));
    auto cursor = field.textCursor();
    cursor.setPosition(15);
    field.setTextCursor(cursor);
    require(lingtai::desktop::replace_composer_spell_word(
            &field, word, QStringLiteral("spelling"))
            && field.textCursor().position() == 16,
        "replacement must shift a cursor after the corrected range coherently");
    require(!lingtai::desktop::replace_composer_spell_word(
            &field, word, QStringLiteral("other")),
        "a stale word range must not overwrite newer composer text");
}

void verify_menu_and_effects() {
    auto state = std::make_shared<FakeSpellState>();
    state->results[QStringLiteral("speling")] = ComposerSpellResult{
        .misspelled = true,
        .suggestions = {
            QStringLiteral("spelling"), QStringLiteral("spieling"),
            QStringLiteral("spelling"), QString(),
        },
    };
    state->results[QStringLiteral("oddball")] = ComposerSpellResult{
        .misspelled = true,
    };
    auto service = make_service(state);

    QWidget host;
    host.resize(520, 180);
    Ui::InputField field(
        &host, st::defaultInputField, Ui::InputField::Mode::MultiLine,
        rpl::single(QStringLiteral("Message")));
    field.setGeometry(10, 10, 480, 100);
    lingtai::desktop::add_composer_spell_context_menu_hook(
        &field, [&service] { return service; });
    host.show();
    QCoreApplication::processEvents();

    field.setText(QStringLiteral("one speling two"));
    auto selection = field.textCursor();
    selection.setPosition(0);
    selection.setPosition(3, QTextCursor::KeepAnchor);
    field.setTextCursor(selection);
    auto *popup = request_menu(field, 7);
    require(state->checked.size() == 1
            && state->checked.back().second == QStringLiteral("speling"),
        "the actual context-menu point must resolve the intended word");
    require(popup && named_action(popup, "lingtai_spell_suggestion_0")
            && named_action(popup, "lingtai_spell_suggestion_1")
            && !named_action(popup, "lingtai_spell_suggestion_2"),
        "misspelling suggestions must be bounded, de-duplicated real actions");
    require(named_action(popup, "lingtai_spell_learn")
            && named_action(popup, "lingtai_spell_ignore"),
        "a misspelling must expose distinct truthful Learn and Ignore actions");
    named_action(popup, "lingtai_spell_suggestion_0")->trigger();
    require(field.getLastText() == QStringLiteral("one spelling two")
            && field.textCursor().selectedText() == QStringLiteral("one"),
        "a suggestion action must replace only the captured word and preserve selection");
    popup->hideMenu(true);

    field.setText(QStringLiteral("speling"));
    popup = request_menu(field, 3);
    named_action(popup, "lingtai_spell_learn")->trigger();
    require(state->learned_calls == std::vector<QString>{QStringLiteral("speling")}
            && state->ignored_calls.empty(),
        "Learn must call the global learning effect, not document Ignore");
    popup->hideMenu(true);
    popup = request_menu(field, 3);
    require(named_action(popup, "lingtai_spell_unlearn")
            && !named_action(popup, "lingtai_spell_ignore"),
        "truthfully learned state must expose Unlearn instead of misspelling effects");
    named_action(popup, "lingtai_spell_unlearn")->trigger();
    require(state->unlearned_calls
            == std::vector<QString>{QStringLiteral("speling")},
        "Unlearn must call the real inverse learning effect");
    popup->hideMenu(true);

    popup = request_menu(field, 3);
    named_action(popup, "lingtai_spell_ignore")->trigger();
    require(state->ignored_calls.size() == 1
            && state->ignored_calls.front().first == state->opened.front()
            && state->learned_calls.size() == 1,
        "Ignore must use this service's document tag without global learning");
    popup->hideMenu(true);
    popup = request_menu(field, 3);
    require(!named_action(popup, "lingtai_spell_suggestion_0")
            && !named_action(popup, "lingtai_spell_learn")
            && !named_action(popup, "lingtai_spell_ignore"),
        "a document-ignored word must leave only the standard menu");
    popup->hideMenu(true);

    field.setText(QStringLiteral("oddball"));
    popup = request_menu(field, 2);
    require(!named_action(popup, "lingtai_spell_suggestion_0")
            && named_action(popup, "lingtai_spell_learn")
            && named_action(popup, "lingtai_spell_ignore"),
        "no guesses must add no fake suggestion row while retaining real effects");
    popup->hideMenu(true);

    const auto checks_before = state->checked.size();
    field.setText(QStringLiteral("🙂 !"));
    popup = request_menu(field, 0);
    require(popup && state->checked.size() == checks_before,
        "a no-word point must keep the standard menu without querying the service");
    popup->hideMenu(true);

    const auto tag = state->opened.front();
    service.reset();
    require(state->closed == std::vector<ComposerSpellDocumentTag>{tag},
        "destroying one service must close its bounded spell-document tag once");
}

void verify_standard_action_states() {
    auto state = std::make_shared<FakeSpellState>();
    auto service = make_service(state);
    QWidget host;
    host.resize(520, 180);
    Ui::InputField field(
        &host, st::defaultInputField, Ui::InputField::Mode::MultiLine,
        rpl::single(QStringLiteral("Message")));
    field.setGeometry(10, 10, 480, 100);
    lingtai::desktop::add_composer_spell_context_menu_hook(
        &field, [&service] { return service; });
    host.show();
    QCoreApplication::processEvents();

    const auto action_enabled = [](const auto &states, const QString &text) {
        const auto found = std::find_if(
            states.begin(), states.end(), [&](const auto &state) {
                return state.text == text;
            });
        require(found != states.end(),
            "the representative standard action must exist");
        return found->enabled;
    };
    auto verify_current_state = [&](int point) {
        auto expected_menu = std::unique_ptr<QMenu>(
            field.rawTextEdit()->createStandardContextMenu());
        const auto expected = standard_actions(expected_menu->actions());
        auto *popup = request_menu(field, point);
        const auto actual = popup
            ? standard_actions(popup) : std::vector<StandardActionState>();
        require(popup && actual == expected,
            "the hook must preserve standard order, shortcuts, and Qt-derived enabled state");
        popup->hideMenu(true);
        return actual;
    };

    field.clear();
    const auto empty = verify_current_state(0);
    require(!action_enabled(empty, QStringLiteral("Undo"))
            && !action_enabled(empty, QStringLiteral("Cut"))
            && !action_enabled(empty, QStringLiteral("Copy"))
            && !action_enabled(empty, QStringLiteral("Delete")),
        "empty composer standard edit actions must retain Qt's disabled state");

    field.setText(QStringLiteral("plain text"));
    const auto text = verify_current_state(2);
    require(action_enabled(text, QStringLiteral("Select All"))
            && !action_enabled(text, QStringLiteral("Cut"))
            && !action_enabled(text, QStringLiteral("Copy")),
        "unselected text must enable Select All without enabling selection actions");

    auto selection = field.textCursor();
    selection.setPosition(0);
    selection.setPosition(5, QTextCursor::KeepAnchor);
    field.setTextCursor(selection);
    const auto selected = verify_current_state(2);
    require(action_enabled(selected, QStringLiteral("Cut"))
            && action_enabled(selected, QStringLiteral("Copy"))
            && action_enabled(selected, QStringLiteral("Delete")),
        "selected text must retain Qt's enabled edit actions");

    field.clear();
    auto cursor = field.textCursor();
    cursor.insertText(QStringLiteral("undoable"));
    field.setTextCursor(cursor);
    const auto undoable = verify_current_state(2);
    require(action_enabled(undoable, QStringLiteral("Undo")),
        "an undoable insertion must retain Qt's enabled Undo action");
}

void verify_pending_destruction() {
    auto state = std::make_shared<FakeSpellState>();
    state->asynchronous = true;
    state->results[QStringLiteral("speling")] = ComposerSpellResult{
        .misspelled = true,
        .suggestions = {QStringLiteral("spelling")},
    };
    auto service = make_service(state);
    auto host = std::make_unique<QWidget>();
    auto field = std::make_unique<Ui::InputField>(
        host.get(), st::defaultInputField, Ui::InputField::Mode::MultiLine,
        rpl::single(QStringLiteral("Message")));
    field->setText(QStringLiteral("speling"));
    lingtai::desktop::add_composer_spell_context_menu_hook(
        field.get(), [&service] { return service; });
    request_menu(*field, 3);
    require(state->pending && !current_popup(*field),
        "an actual async check must defer the one styled popup");
    const auto tag = state->opened.front();
    field.reset();
    host.reset();
    service.reset();
    require(state->closed == std::vector<ComposerSpellDocumentTag>{tag},
        "field destruction during a request must still clean up its session tag");
    auto completion = std::move(state->pending);
    completion(std::move(state->pending_result));
    QCoreApplication::processEvents();

    state = std::make_shared<FakeSpellState>();
    state->asynchronous = true;
    state->results[QStringLiteral("speling")] = ComposerSpellResult{
        .misspelled = true,
        .suggestions = {QStringLiteral("spelling")},
    };
    service = make_service(state);
    QWidget second_host;
    Ui::InputField second_field(
        &second_host, st::defaultInputField, Ui::InputField::Mode::MultiLine,
        rpl::single(QStringLiteral("Message")));
    second_field.setText(QStringLiteral("speling"));
    auto pending_menu = QPointer<QMenu>();
    second_field.addContextMenuHook(
        [&pending_menu](Ui::InputField::ContextMenuRequest request) {
            pending_menu = request.menu;
        });
    lingtai::desktop::add_composer_spell_context_menu_hook(
        &second_field, [&service] { return service; });
    request_menu(second_field, 3);
    require(pending_menu && state->pending,
        "the pending-menu lifetime fixture must capture the standard QMenu");
    delete pending_menu.data();
    auto menu_completion = std::move(state->pending);
    menu_completion(std::move(state->pending_result));
    QCoreApplication::processEvents();
    require(!current_popup(second_field),
        "destroying the pending QMenu must not show or mutate a dead menu");
}

QStringList captured_messages;

void capture_message(
        QtMsgType,
        const QMessageLogContext &,
        const QString &message) {
    captured_messages.push_back(message);
}

void verify_no_content_logging() {
    captured_messages.clear();
    const auto previous = qInstallMessageHandler(capture_message);
    auto state = std::make_shared<FakeSpellState>();
    const auto secret_word = QStringLiteral("privateword-c4e8");
    const auto secret_suggestion = QStringLiteral("privatesuggestion-a91f");
    state->results[secret_word] = ComposerSpellResult{
        .misspelled = true,
        .suggestions = {secret_suggestion},
    };
    auto service = make_service(state);
    service->check(secret_word, [](ComposerSpellResult) {});
    service->learn(secret_word);
    service->ignore(secret_word);
    service->unlearn(secret_word);
    qInstallMessageHandler(previous);
    for (const auto &message : captured_messages) {
        require(!message.contains(secret_word)
                && !message.contains(secret_suggestion),
            "spell operations must never log candidate or suggestion content");
    }
}

} // namespace

int main(int argc, char **argv) {
    try {
        QApplication application(argc, argv);
        lingtai::desktop::ui_test::applyUiTestFontDefaults();
        auto runtime = TestUiRuntime();
        verify_word_ranges();
        verify_replacement_mapping();
        verify_menu_and_effects();
        verify_standard_action_states();
        verify_pending_destruction();
        verify_no_content_logging();
        std::cout << "composer spellcheck behavior: OK\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "composer spellcheck behavior: " << error.what() << '\n';
        return 1;
    }
}
