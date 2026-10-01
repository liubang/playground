// Copyright (c) 2026 The Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import AppKit
import SwiftUI

/// The composer's text editor: a hand-rolled NSTextView instead of
/// SwiftUI's TextEditor. Two AppKit behaviors made the TextEditor port
/// read as "off" next to the WebUI's textarea:
///
/// 1. The default caret is a 2pt bar spanning the FULL line fragment
///    (glyphs plus the 5pt line spacing), drawn straddling the insertion
///    point — so it sliced through the placeholder's first glyph, which
///    lived in a separate SwiftUI layer aligned by hand-tuned paddings.
///    Here the caret is slimmed to 1.5pt and trimmed to the glyph line,
///    and the placeholder is drawn INSIDE the text view at the text
///    container's own origin: caret, placeholder, and typed text share
///    one coordinate space by construction.
/// 2. Focus/IME/key handling needed hacks (a firstResponder lookup just
///    to read hasMarkedText). Owning the NSTextView makes the Return
///    semantics (bare = submit, ⇧ = newline, ⌘ = submit) and the IME-
///    composition guard direct overrides of keyDown(with:).
///
/// Geometry mirrors the WebUI's .composer textarea: 5pt leading inset,
/// 6pt top/bottom inset, 14pt system font, 5pt line spacing (≈ the
/// WebUI's 1.55 line-height), growing from 44 to 200pt before the
/// overlay scroller engages (the frame clamp lives on the SwiftUI side).
struct ComposerTextView: NSViewRepresentable {
    @Binding var text: String
    /// The @ScaledMetric-resolved point size (Dynamic Type aware).
    var fontSize: CGFloat
    var placeholder: String
    /// Increment to make the text view first responder (appear, session
    /// switch). NOT a FocusState: an unattached @FocusState binding is
    /// reset to false by SwiftUI's focus engine on every re-render (the
    /// halo died on the first keystroke), so focus is driven explicitly.
    var focusRequest: Int
    /// The text view's first-responder changes (drives the halo).
    var onFocusChange: (Bool) -> Void
    /// Bare-Return handler: return true when the keystroke was consumed
    /// (message sent); false falls through to inserting a newline — the
    /// old onKeyPress's .ignored.
    var onSubmit: () -> Bool

    func makeCoordinator() -> Coordinator {
        Coordinator(self)
    }

    func makeNSView(context: Context) -> ComposerScrollView {
        let scrollView = ComposerScrollView()
        // init(frame:textContainer: nil) does NOT build a text system on
        // modern macOS — layoutManager/textContainer/textStorage all stay
        // nil and every edit silently no-ops (the "can't type" bug).
        // Build the TextKit stack explicitly.
        let textStorage = NSTextStorage()
        let layoutManager = NSLayoutManager()
        let textContainer = NSTextContainer(
            size: NSSize(width: 0, height: CGFloat.greatestFiniteMagnitude),
        )
        textStorage.addLayoutManager(layoutManager)
        layoutManager.addTextContainer(textContainer)
        let textView = ComposerNSTextView(frame: .zero, textContainer: textContainer)

        textView.drawsBackground = false
        textView.isRichText = false
        textView.allowsUndo = true
        textView.textContainerInset = NSSize(width: 5, height: 6)
        textContainer.lineFragmentPadding = 0
        textView.isVerticallyResizable = true
        textView.isHorizontallyResizable = false
        textView.autoresizingMask = [.width]
        textContainer.widthTracksTextView = true
        textView.minSize = .zero
        textView.maxSize = NSSize(
            width: CGFloat.greatestFiniteMagnitude, height: CGFloat.greatestFiniteMagnitude,
        )
        textView.insertionPointColor = Theme.AppKit.fg
        textView.selectedTextAttributes = [
            .backgroundColor: Theme.AppKit.fg.withAlphaComponent(0.3),
        ]
        // A chat composer, not Mail: substitutions must never rewrite
        // what the user typed (smart quotes break pasted code).
        textView.isAutomaticQuoteSubstitutionEnabled = false
        textView.isAutomaticDashSubstitutionEnabled = false
        textView.isAutomaticTextReplacementEnabled = false
        textView.isAutomaticSpellingCorrectionEnabled = false
        textView.delegate = context.coordinator

        scrollView.documentView = textView
        scrollView.composerTextView = textView
        scrollView.drawsBackground = false
        scrollView.borderType = .noBorder
        scrollView.hasVerticalScroller = true
        scrollView.autohidesScrollers = true
        scrollView.scrollerStyle = .overlay
        // The SwiftUI accessibility modifiers land on the scroll view;
        // VoiceOver's text area is the text view.
        textView.setAccessibilityLabel("Message")
        textView.setAccessibilityPlaceholderValue(placeholder)
        return scrollView
    }

    func updateNSView(_ nsView: ComposerScrollView, context: Context) {
        guard let textView = nsView.composerTextView else { return }
        context.coordinator.parent = self
        textView.onSubmit = onSubmit

        if textView.placeholder != placeholder {
            textView.placeholder = placeholder
            textView.setAccessibilityPlaceholderValue(placeholder)
        }
        if textView.contentFont.pointSize != fontSize {
            textView.contentFont = .systemFont(ofSize: fontSize)
            nsView.invalidateIntrinsicContentSize()
        }
        if textView.string != text, !textView.hasMarkedText() {
            // External edit: send cleared the draft, "continue unfinished
            // task" seeded one, a queued steer appended to it. Replace
            // wholesale and park the caret at the end.
            textView.textStorage?.setAttributedString(
                NSAttributedString(string: text, attributes: textView.contentAttributes),
            )
            textView.setSelectedRange(NSRange(location: text.utf16.count, length: 0))
            nsView.invalidateIntrinsicContentSize()
        }

        nsView.desiredFocus = nsView.lastFocusRequest != focusRequest
        if nsView.desiredFocus {
            nsView.lastFocusRequest = focusRequest
            if let window = nsView.window, window.firstResponder !== textView {
                // makeFirstResponder inside a view update is re-entrant; defer.
                DispatchQueue.main.async {
                    window.makeFirstResponder(textView)
                }
            }
        }
    }

    final class Coordinator: NSObject, NSTextViewDelegate {
        var parent: ComposerTextView

        init(_ parent: ComposerTextView) {
            self.parent = parent
        }

        func textDidChange(_ notification: Notification) {
            guard let textView = notification.object as? ComposerNSTextView,
                  let scrollView = textView.enclosingScrollView as? ComposerScrollView
            else { return }
            // Never push IME preedit into the draft: the romaji run would
            // arm the send button — and its 0.3s spring — on EVERY
            // composition keystroke, and could leak into a sent message.
            // The commit edit arrives unmarked and pushes in full.
            if !textView.hasMarkedText() {
                parent.text = textView.string
            }
            // SwiftUI layout is only needed when the content HEIGHT moved
            // — per-keystroke invalidation for same-line typing is pure
            // churn (the caret stuttered under it).
            scrollView.invalidateIntrinsicContentSizeIfContentHeightChanged()
        }

        func textDidBeginEditing(_: Notification) {
            parent.onFocusChange(true)
        }

        func textDidEndEditing(_: Notification) {
            parent.onFocusChange(false)
        }
    }
}

// MARK: - Scroll view (intrinsic height = content height)

/// The NSScrollView whose intrinsic height tracks the laid-out text:
/// SwiftUI's frame(minHeight:maxHeight:) clamp provides the 44–200pt
/// window, and past 200 the overlay scroller takes over.
final class ComposerScrollView: NSScrollView {
    weak var composerTextView: ComposerNSTextView?
    /// A focus request observed while the view had no window yet,
    /// applied in viewDidMoveToWindow.
    var desiredFocus = false
    /// The focus token last honored (see ComposerTextView.focusRequest).
    var lastFocusRequest = 0

    private var lastLayoutWidth: CGFloat = -1
    private var lastContentHeight: CGFloat = -1

    override var intrinsicContentSize: NSSize {
        guard let textView = composerTextView,
              let layoutManager = textView.layoutManager,
              let textContainer = textView.textContainer
        else { return super.intrinsicContentSize }
        layoutManager.ensureLayout(for: textContainer)
        let used = layoutManager.usedRect(for: textContainer)
        let height = ceil(used.height + textView.textContainerInset.height * 2)
        lastContentHeight = height
        return NSSize(width: NSView.noIntrinsicMetric, height: height)
    }

    /// textDidChange fires per keystroke; only height changes justify a
    /// SwiftUI layout pass.
    func invalidateIntrinsicContentSizeIfContentHeightChanged() {
        guard let textView = composerTextView,
              let layoutManager = textView.layoutManager,
              let textContainer = textView.textContainer
        else { return }
        layoutManager.ensureLayout(for: textContainer)
        let used = layoutManager.usedRect(for: textContainer)
        let height = ceil(used.height + textView.textContainerInset.height * 2)
        if height != lastContentHeight {
            lastContentHeight = height
            invalidateIntrinsicContentSize()
        }
    }

    override func layout() {
        super.layout()
        // A width change re-wraps the text, which changes the height.
        if bounds.width != lastLayoutWidth {
            lastLayoutWidth = bounds.width
            invalidateIntrinsicContentSize()
        }
    }

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        guard window != nil, desiredFocus, let textView = composerTextView else { return }
        DispatchQueue.main.async { [weak self] in
            self?.window?.makeFirstResponder(textView)
        }
    }
}

// MARK: - Text view (caret, placeholder, keys)

final class ComposerNSTextView: NSTextView {
    /// ≈ the WebUI's 1.55 line-height at 14px.
    static let lineSpacing: CGFloat = 5

    /// See ComposerTextView.onSubmit.
    var onSubmit: (() -> Bool)?

    var contentFont: NSFont = .systemFont(ofSize: 14) {
        didSet { applyContentAttributes() }
    }

    /// Drawn inside the text container's own origin so it shares the
    /// caret's and typed text's coordinate space exactly — no hand-tuned
    /// overlay paddings. tokens.css: placeholder = muted at 70%. The 1pt
    /// leading offset keeps the (insertion-point-centered) caret from
    /// touching the first hint glyph; against typed text the offset is
    /// imperceptible since the placeholder is gone by then.
    var placeholder = "" {
        didSet { needsDisplay = true }
    }

    override init(frame frameRect: NSRect, textContainer container: NSTextContainer?) {
        super.init(frame: frameRect, textContainer: container)
        applyContentAttributes()
    }

    @available(*, unavailable)
    required init?(coder _: NSCoder) {
        fatalError("init(coder:) is not supported")
    }

    var contentAttributes: [NSAttributedString.Key: Any] {
        let style = NSMutableParagraphStyle()
        style.lineSpacing = Self.lineSpacing
        return [
            .font: contentFont,
            .foregroundColor: Theme.AppKit.fg,
            .paragraphStyle: style,
        ]
    }

    private func applyContentAttributes() {
        typingAttributes = contentAttributes
        font = contentFont
        textColor = Theme.AppKit.fg
        // IME preedit: themed single underline, NOT the default gold
        // highlight, which clashes badly with the dark Everforest palette.
        markedTextAttributes = [
            .font: contentFont,
            .foregroundColor: Theme.AppKit.fg,
            .backgroundColor: NSColor.clear,
            .underlineStyle: NSUnderlineStyle.single.rawValue,
            .underlineColor: Theme.AppKit.muted,
        ]
        if let storage = textStorage, storage.length > 0 {
            storage.setAttributes(
                contentAttributes,
                range: NSRange(location: 0, length: storage.length),
            )
        }
        needsDisplay = true
    }

    override func draw(_ dirtyRect: NSRect) {
        super.draw(dirtyRect)
        guard string.isEmpty, !hasMarkedText(), !placeholder.isEmpty else { return }
        let style = NSMutableParagraphStyle()
        style.lineSpacing = Self.lineSpacing
        style.lineBreakMode = .byTruncatingTail
        let attributes: [NSAttributedString.Key: Any] = [
            .font: contentFont,
            .foregroundColor: Theme.AppKit.muted.withAlphaComponent(0.7),
            .paragraphStyle: style,
        ]
        let inset = textContainerInset
        let fragmentPadding = textContainer?.lineFragmentPadding ?? 0
        let rect = NSRect(
            x: inset.width + fragmentPadding + 1,
            y: inset.height,
            width: bounds.width - (inset.width + fragmentPadding) * 2 - 1,
            height: bounds.height - inset.height * 2,
        )
        NSString(string: placeholder).draw(
            with: rect,
            options: [.usesLineFragmentOrigin, .truncatesLastVisibleLine],
            attributes: attributes,
        )
    }

    /// Caret geometry, rebuilt. Horizontal: sit EXACTLY on the insertion
    /// point (AppKit passes the rect centered on it) — typed glyphs must
    /// land on the caret; an earlier revision shifted the bar left of the
    /// insertion point to dodge the placeholder's first glyph, which made
    /// it read as detached from the text (the placeholder now keeps clear
    /// via its own 1pt leading offset in draw(_:) instead). Vertical: the
    /// glyphs' own line height, never the whole line fragment — non-final
    /// fragments include the paragraph's 5pt spacing (22pt), while final
    /// and empty-document fragments don't (17pt), and line-boundary rects
    /// can arrive degenerate (height 1); fall back to the font's
    /// ascender…descender whenever the passed rect isn't a full line.
    override func drawInsertionPoint(in rect: NSRect, color: NSColor, turnedOn flag: Bool) {
        var caret = rect
        caret.size.width = 1.5
        caret.origin.x = rect.midX - caret.size.width / 2
        let glyphHeight = contentFont.ascender - contentFont.descender
        if rect.height > glyphHeight + 2 {
            caret.size.height = rect.height - Self.lineSpacing
        } else {
            caret.size.height = max(rect.height, glyphHeight)
        }
        if let scale = window?.backingScaleFactor {
            caret.origin.x = (caret.origin.x * scale).rounded() / scale
        }
        super.drawInsertionPoint(in: caret, color: color, turnedOn: flag)
    }

    override func setSelectedRanges(
        _ ranges: [NSValue],
        affinity: NSSelectionAffinity,
        stillSelecting stillSelectingFlag: Bool,
    ) {
        super.setSelectedRanges(
            ranges,
            affinity: affinity,
            stillSelecting: stillSelectingFlag,
        )
        if !stillSelectingFlag {
            // NSTextView does NOT recompute its cached insertion-point rect
            // synchronously on a selection change — the recompute is
            // deferred past the next display pass, which therefore draws
            // the caret ON at the PRE-edit position first and only then at
            // the correct one. In a SwiftUI host the two passes straddle a
            // window flush, so the stale caret hits the screen for a frame:
            // under key repeat (e.g. held Delete) the caret visibly trails
            // one glyph behind the text. Force the recompute NOW, while
            // text and layout are final. (Diagnosed via instrumented
            // drawInsertionPoint logging: every keystroke produced a
            // stale-then-correct draw pair; this call collapses it to one
            // correct draw.)
            updateInsertionPointStateAndRestartTimer(true)
        }
    }

    /// IME preedit goes through here. Third-party IMEs (搜狗/微信/…)
    /// frequently pass ATTRIBUTED runs with hardcoded dark foregrounds —
    /// invisible on our dark surface, which read as "the caret moves but
    /// nothing types" until the commit lands. Normalize to the plain
    /// string so markedTextAttributes always styles preedit. (Apple's own
    /// Pinyin already takes the plain-string path; unaffected.)
    override func setMarkedText(
        _ string: Any,
        selectedRange: NSRange,
        replacementRange: NSRange,
    ) {
        if let attributed = string as? NSAttributedString {
            super.setMarkedText(
                attributed.string,
                selectedRange: selectedRange,
                replacementRange: replacementRange,
            )
        } else {
            super.setMarkedText(
                string,
                selectedRange: selectedRange,
                replacementRange: replacementRange,
            )
        }
    }

    /// Return semantics, decided from the event itself (reading
    /// NSApp.currentEvent inside doCommand(by:) is fragile — the two can
    /// diverge). Bare Return submits (or falls through to a newline when
    /// onSubmit declines — the old .ignored); ⇧Return always inserts a
    /// newline (NSTextView's default key bindings map it to NOTHING);
    /// ⌘Return submits too and must never insert a newline (it surfaces
    /// when the send button — and its shortcut — is disabled); ⌥Return
    /// keeps the standard binding (insertNewlineIgnoringFieldEditor).
    /// A Return that CONFIRMS an IME candidate is untouched (hasMarkedText).
    override func keyDown(with event: NSEvent) {
        let isReturn = event.keyCode == 36 || event.keyCode == 76 // return + keypad enter
        let modifiers = event.modifierFlags.intersection(.deviceIndependentFlagsMask)
        if isReturn, !hasMarkedText() {
            switch modifiers {
            case []:
                if onSubmit?() == true {
                    return
                }
            case [.shift]:
                insertText("\n", replacementRange: selectedRange())
                return
            case [.command]:
                _ = onSubmit?()
                return
            default:
                break
            }
        }
        super.keyDown(with: event)
    }
}
