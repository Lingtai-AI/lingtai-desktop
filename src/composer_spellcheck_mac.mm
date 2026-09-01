#include "composer_spellcheck.h"

#import <AppKit/AppKit.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QMetaObject>

#include <memory>
#include <set>
#include <utility>

namespace lingtai::desktop {
namespace {

class MacComposerSpellPlatform final
    : public ComposerSpellPlatform
    , public std::enable_shared_from_this<MacComposerSpellPlatform> {
public:
    ComposerSpellDocumentTag open_document() override {
        const auto tag = static_cast<ComposerSpellDocumentTag>(
            [NSSpellChecker uniqueSpellDocumentTag]);
        active_tags_.insert(tag);
        return tag;
    }

    void close_document(ComposerSpellDocumentTag tag) override {
        if (active_tags_.erase(tag) != 0) {
            // A fresh unique tag has no NSSpellChecker-owned state yet. Avoid
            // initializing the platform singleton during unrelated composer
            // teardown; close only sessions this adapter actually checked or
            // ignored.
            if (used_tags_.erase(tag) != 0) {
                [[NSSpellChecker sharedSpellChecker]
                    closeSpellDocumentWithTag:static_cast<NSInteger>(tag)];
            }
        }
    }

    bool checks_asynchronously() const noexcept override {
        return true;
    }

    void check(
            ComposerSpellDocumentTag tag,
            const QString &word,
            ComposerSpellCompletion completion) override {
        used_tags_.insert(tag);
        auto *checker = [NSSpellChecker sharedSpellChecker];
        auto *native_word = word.toNSString();
        const auto range = NSMakeRange(0, native_word.length);
        const auto weak = weak_from_this();
        [checker requestCheckingOfString:native_word
            range:range
            types:NSTextCheckingTypeSpelling
            options:nil
            inSpellDocumentWithTag:static_cast<NSInteger>(tag)
            completionHandler:^(
                NSInteger,
                NSArray<NSTextCheckingResult *> *results,
                NSOrthography *,
                NSInteger) {
                auto misspelled = false;
                for (NSTextCheckingResult *result in results) {
                    if (result.resultType == NSTextCheckingTypeSpelling) {
                        misspelled = true;
                        break;
                    }
                }
                auto *application = QCoreApplication::instance();
                if (!application) {
                    return;
                }
                QMetaObject::invokeMethod(application,
                    [weak, tag, word, misspelled,
                     completion = std::move(completion)]() mutable {
                        const auto self = weak.lock();
                        if (!self || !self->active_tags_.contains(tag)) {
                            return;
                        }
                        auto *checker = [NSSpellChecker sharedSpellChecker];
                        auto *native_word = word.toNSString();
                        const auto learned = static_cast<bool>(
                            [checker hasLearnedWord:native_word]);
                        auto result = ComposerSpellResult{
                            .misspelled = misspelled && !learned,
                            .learned = learned,
                        };
                        if (result.misspelled) {
                            const auto range = NSMakeRange(
                                0, native_word.length);
                            auto *guesses = [checker
                                guessesForWordRange:range
                                inString:native_word
                                language:nil
                                inSpellDocumentWithTag:
                                    static_cast<NSInteger>(tag)];
                            for (NSString *guess in guesses) {
                                result.suggestions.push_back(
                                    QString::fromNSString(guess));
                            }
                        }
                        completion(std::move(result));
                    },
                    Qt::QueuedConnection);
            }];
    }

    void learn(const QString &word) override {
        [[NSSpellChecker sharedSpellChecker] learnWord:word.toNSString()];
    }

    void unlearn(const QString &word) override {
        [[NSSpellChecker sharedSpellChecker] unlearnWord:word.toNSString()];
    }

    void ignore(
            ComposerSpellDocumentTag tag,
            const QString &word) override {
        if (active_tags_.contains(tag)) {
            used_tags_.insert(tag);
            [[NSSpellChecker sharedSpellChecker]
                ignoreWord:word.toNSString()
                inSpellDocumentWithTag:static_cast<NSInteger>(tag)];
        }
    }

private:
    std::set<ComposerSpellDocumentTag> active_tags_;
    std::set<ComposerSpellDocumentTag> used_tags_;
};

} // namespace

std::shared_ptr<ComposerSpellService> make_platform_composer_spell_service() {
    return std::make_shared<ComposerSpellService>(
        std::make_shared<MacComposerSpellPlatform>());
}

} // namespace lingtai::desktop
