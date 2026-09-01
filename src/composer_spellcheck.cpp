#include "composer_spellcheck.h"

#include "base/basic_types.h"
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

#include <QtCore/QPointer>
#include <QtGui/QAction>
#include <QtGui/QContextMenuEvent>
#include <QtGui/QTextCursor>
#include <QtGui/QTextDocument>
#include <QtWidgets/QMenu>
#include <QtWidgets/QTextEdit>

#include <algorithm>
#include <utility>

namespace lingtai::desktop {
namespace {

constexpr auto kMaximumSuggestions = 6;

[[nodiscard]] bool contains_word_character(const QString &text) {
    for (const auto character : text.toUcs4()) {
        const auto category = QChar::category(character);
        if (QChar::isLetterOrNumber(character)
            || category == QChar::Mark_NonSpacing
            || category == QChar::Mark_SpacingCombining
            || category == QChar::Mark_Enclosing) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] int mapped_position(
        int position,
        const ComposerSpellWord &word,
        int replacement_length) {
    if (position <= word.from) {
        return position;
    }
    if (position >= word.till) {
        return position + replacement_length - (word.till - word.from);
    }
    return word.from + std::min(position - word.from, replacement_length);
}

void add_spell_actions(
        QMenu *menu,
        Ui::InputField *field,
        std::weak_ptr<ComposerSpellService> weak_service,
        ComposerSpellWord word,
        ComposerSpellResult result) {
    auto suggestions = QStringList();
    for (const auto &suggestion : result.suggestions) {
        if (suggestion.isEmpty()
            || suggestion == word.text
            || suggestions.contains(suggestion)) {
            continue;
        }
        suggestions.push_back(suggestion);
        if (suggestions.size() == kMaximumSuggestions) {
            break;
        }
    }

    const auto has_misspelling_actions = result.misspelled;
    const auto has_unlearn_action = result.learned;
    if (!has_misspelling_actions && !has_unlearn_action) {
        return;
    }

    auto *before = menu->actions().empty() ? nullptr : menu->actions().front();
    auto insert = [menu, before](QAction *action) {
        if (before) {
            menu->insertAction(before, action);
        } else {
            menu->addAction(action);
        }
    };
    auto guarded_field = QPointer<Ui::InputField>(field);

    if (result.misspelled) {
        for (auto index = 0; index != suggestions.size(); ++index) {
            auto *action = new QAction(suggestions[index], menu);
            action->setObjectName(
                QStringLiteral("lingtai_spell_suggestion_%1").arg(index));
            const auto replacement = suggestions[index];
            QObject::connect(action, &QAction::triggered, menu,
                [guarded_field, word, replacement] {
                    if (guarded_field) {
                        static_cast<void>(replace_composer_spell_word(
                            guarded_field, word, replacement));
                    }
                });
            insert(action);
        }

        auto *learn = new QAction(QStringLiteral("Learn Spelling"), menu);
        learn->setObjectName(QStringLiteral("lingtai_spell_learn"));
        QObject::connect(learn, &QAction::triggered, menu,
            [weak_service, word] {
                if (const auto service = weak_service.lock()) {
                    service->learn(word.text);
                }
            });
        insert(learn);

        auto *ignore = new QAction(QStringLiteral("Ignore Spelling"), menu);
        ignore->setObjectName(QStringLiteral("lingtai_spell_ignore"));
        QObject::connect(ignore, &QAction::triggered, menu,
            [weak_service, word] {
                if (const auto service = weak_service.lock()) {
                    service->ignore(word.text);
                }
            });
        insert(ignore);
    } else if (result.learned) {
        auto *unlearn = new QAction(QStringLiteral("Unlearn Spelling"), menu);
        unlearn->setObjectName(QStringLiteral("lingtai_spell_unlearn"));
        QObject::connect(unlearn, &QAction::triggered, menu,
            [weak_service, word] {
                if (const auto service = weak_service.lock()) {
                    service->unlearn(word.text);
                }
            });
        insert(unlearn);
    }

    if (before) {
        menu->insertSeparator(before);
    }
}

} // namespace

ComposerSpellService::ComposerSpellService(
    std::shared_ptr<ComposerSpellPlatform> platform)
    : platform_(std::move(platform))
    , alive_(std::make_shared<std::atomic_bool>(true))
    , tag_(platform_ ? platform_->open_document() : 0) {
}

ComposerSpellService::~ComposerSpellService() {
    alive_->store(false, std::memory_order_relaxed);
    if (platform_) {
        platform_->close_document(tag_);
    }
}

bool ComposerSpellService::checks_asynchronously() const noexcept {
    return platform_ && platform_->checks_asynchronously();
}

void ComposerSpellService::check(
        const QString &word,
        ComposerSpellCompletion completion) {
    if (!platform_) {
        completion({});
        return;
    }
    const auto alive = std::weak_ptr<std::atomic_bool>(alive_);
    platform_->check(tag_, word,
        [alive, completion = std::move(completion)](
                ComposerSpellResult result) mutable {
            const auto guard = alive.lock();
            if (guard && guard->load(std::memory_order_relaxed)) {
                completion(std::move(result));
            }
        });
}

void ComposerSpellService::learn(const QString &word) {
    if (platform_) {
        platform_->learn(word);
    }
}

void ComposerSpellService::unlearn(const QString &word) {
    if (platform_) {
        platform_->unlearn(word);
    }
}

void ComposerSpellService::ignore(const QString &word) {
    if (platform_) {
        platform_->ignore(tag_, word);
    }
}

std::optional<ComposerSpellWord> composer_spell_word_at(
        QTextDocument *document,
        int document_position) {
    if (!document) {
        return std::nullopt;
    }
    const auto maximum = std::max(0, document->characterCount() - 1);
    const auto position = std::clamp(document_position, 0, maximum);
    const auto candidates = {position, position - 1, position + 1};
    for (const auto candidate : candidates) {
        if (candidate < 0 || candidate > maximum) {
            continue;
        }
        auto cursor = QTextCursor(document);
        cursor.setPosition(candidate);
        cursor.select(QTextCursor::WordUnderCursor);
        const auto text = cursor.selectedText();
        if (cursor.hasSelection() && contains_word_character(text)) {
            return ComposerSpellWord{
                .text = text,
                .from = cursor.selectionStart(),
                .till = cursor.selectionEnd(),
            };
        }
    }
    return std::nullopt;
}

bool replace_composer_spell_word(
        Ui::InputField *field,
        const ComposerSpellWord &word,
        const QString &replacement) {
    if (!field || replacement.isEmpty() || word.from < 0
        || word.till <= word.from
        || word.till > field->document()->characterCount() - 1) {
        return false;
    }
    auto replace = QTextCursor(field->document());
    replace.setPosition(word.from);
    replace.setPosition(word.till, QTextCursor::KeepAnchor);
    if (replace.selectedText() != word.text) {
        return false;
    }

    const auto original = field->textCursor();
    const auto anchor = original.anchor();
    const auto position = original.position();
    replace.insertText(replacement);

    auto restored = QTextCursor(field->document());
    restored.setPosition(mapped_position(
        anchor, word, replacement.size()));
    restored.setPosition(mapped_position(
        position, word, replacement.size()), QTextCursor::KeepAnchor);
    field->setTextCursor(restored);
    return true;
}

void add_composer_spell_context_menu_hook(
        Ui::InputField *field,
        ComposerSpellServiceProvider service_provider) {
    if (!field) {
        return;
    }
    field->addContextMenuHook(
        [guarded_field = QPointer<Ui::InputField>(field),
         service_provider = std::move(service_provider)](
                Ui::InputField::ContextMenuRequest request) {
            if (!guarded_field) {
                return;
            }
            const auto service = service_provider();
            if (!service) {
                return;
            }
            auto *editor = guarded_field->rawTextEdit().get();
            const auto viewport_point = editor->viewport()->mapFromGlobal(
                request.event->globalPos());
            const auto point_cursor = editor->cursorForPosition(
                viewport_point);
            const auto word = composer_spell_word_at(
                guarded_field->document(), point_cursor.position());
            if (!word) {
                return;
            }

            auto done = service->checks_asynchronously()
                ? request.awaitAsyncWork()
                : Fn<void()>();
            auto guarded_menu = QPointer<QMenu>(request.menu.get());
            const auto weak_service =
                std::weak_ptr<ComposerSpellService>(service);
            service->check(word->text,
                [guarded_field, guarded_menu, weak_service, word = *word,
                 done = std::move(done)](
                        ComposerSpellResult result) mutable {
                    if (!guarded_field || !guarded_menu) {
                        return;
                    }
                    add_spell_actions(
                        guarded_menu,
                        guarded_field,
                        weak_service,
                        word,
                        std::move(result));
                    if (done) {
                        done();
                    }
                });
        });
}

} // namespace lingtai::desktop
