#pragma once

#include <QtCore/QString>
#include <QtCore/QStringList>

#include <cstdint>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>

class QTextDocument;

namespace Ui {
class InputField;
} // namespace Ui

namespace lingtai::desktop {

struct ComposerSpellWord {
    QString text;
    int from = 0;
    int till = 0;
};

struct ComposerSpellResult {
    bool misspelled = false;
    bool learned = false;
    QStringList suggestions;
};

using ComposerSpellDocumentTag = std::int64_t;
using ComposerSpellCompletion =
    std::function<void(ComposerSpellResult result)>;

// A narrow platform boundary. One service owns one platform document tag;
// learning is global while ignore is scoped to that tag.
class ComposerSpellPlatform {
public:
    virtual ~ComposerSpellPlatform() = default;

    [[nodiscard]] virtual ComposerSpellDocumentTag open_document() = 0;
    virtual void close_document(ComposerSpellDocumentTag tag) = 0;
    [[nodiscard]] virtual bool checks_asynchronously() const noexcept = 0;
    virtual void check(
        ComposerSpellDocumentTag tag,
        const QString &word,
        ComposerSpellCompletion completion) = 0;
    virtual void learn(const QString &word) = 0;
    virtual void unlearn(const QString &word) = 0;
    virtual void ignore(
        ComposerSpellDocumentTag tag,
        const QString &word) = 0;
};

class ComposerSpellService final {
public:
    explicit ComposerSpellService(
        std::shared_ptr<ComposerSpellPlatform> platform);
    ~ComposerSpellService();

    ComposerSpellService(const ComposerSpellService &) = delete;
    ComposerSpellService &operator=(const ComposerSpellService &) = delete;

    [[nodiscard]] bool checks_asynchronously() const noexcept;
    void check(const QString &word, ComposerSpellCompletion completion);
    void learn(const QString &word);
    void unlearn(const QString &word);
    void ignore(const QString &word);

private:
    std::shared_ptr<ComposerSpellPlatform> platform_;
    std::shared_ptr<std::atomic_bool> alive_;
    ComposerSpellDocumentTag tag_ = 0;
};

using ComposerSpellServiceProvider =
    std::function<std::shared_ptr<ComposerSpellService>()>;

[[nodiscard]] std::shared_ptr<ComposerSpellService>
make_platform_composer_spell_service();

[[nodiscard]] std::optional<ComposerSpellWord> composer_spell_word_at(
    QTextDocument *document,
    int document_position);

[[nodiscard]] bool replace_composer_spell_word(
    Ui::InputField *field,
    const ComposerSpellWord &word,
    const QString &replacement);

void add_composer_spell_context_menu_hook(
    Ui::InputField *field,
    ComposerSpellServiceProvider service_provider);

} // namespace lingtai::desktop
