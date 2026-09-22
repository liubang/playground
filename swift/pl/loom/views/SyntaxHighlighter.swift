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
import JavaScriptCore
import SwiftUI

/// Code-block syntax highlighting, driven by the SAME engine as the
/// WebUI: the highlight.js bundle (embedded as an app resource,
/// version-matched to webui/package.json) evaluated once inside a
/// JavaScriptCore context. hljs emits a tiny, fixed HTML dialect —
/// only `<span class="hljs-*">` wraps plus five escaped entities — so
/// the HTML → attributed-string conversion is a small stack parser
/// rather than the full (and slow) AppKit HTML importer. Token colors
/// map one-to-one onto the WebUI's code.css classes, so a fenced
/// block renders identically on both clients.
@MainActor
enum SyntaxHighlighter {
    private static let bridge = HighlightBridge()

    /// `code` as attributed text: hljs-highlighted when the language
    /// is known, plain mono otherwise. Results are memoized — SwiftUI
    /// re-invokes body on every transcript frame while most blocks
    /// stay unchanged.
    static func attributed(_ code: String, language: String?) -> AttributedString {
        bridge.attributed(code, language: language)
    }

    /// The unhighlighted rendering (mono + fg). Used by live streaming
    /// code blocks, where the growing unterminated block would
    /// otherwise re-run the JS highlighter every frame on the main
    /// thread.
    static func plain(_ code: String) -> AttributedString {
        HighlightBridge.plain(code)
    }
}

// MARK: - JavaScriptCore bridge

@MainActor
private final class HighlightBridge {
    /// JavaScriptCore contexts are single-threaded; this bridge only
    /// ever runs on the main actor (SwiftUI view bodies).
    private let context: JSContext?
    private let cache = NSCache<NSString, NSAttributedString>()

    init() {
        cache.countLimit = 200
        guard let url = Bundle.main.url(forResource: "highlight.min", withExtension: "js"),
              let source = try? String(contentsOf: url, encoding: .utf8),
              let context = JSContext()
        else {
            context = nil
            return
        }
        context.exceptionHandler = { _, _ in } // a bad snippet must never take down rendering
        context.evaluateScript(source)
        self.context = context
    }

    func attributed(_ code: String, language: String?) -> AttributedString {
        guard let context, let language = Self.normalizedLanguage(language) else {
            return Self.plain(code)
        }
        let key = "\(language)\u{1}\(code)" as NSString
        if let cached = cache.object(forKey: key) {
            return AttributedString(cached)
        }
        guard let html = highlight(code, language: language, in: context) else {
            return Self.plain(code)
        }
        let highlighted = HLJSHTMLConverter.convert(html)
        cache.setObject(highlighted, forKey: key)
        return AttributedString(highlighted)
    }

    private func highlight(_ code: String, language: String, in context: JSContext) -> String? {
        guard let hljs = context.objectForKeyedSubscript("hljs"),
              hljs.invokeMethod("getLanguage", withArguments: [language])?.isUndefined == false,
              let result = hljs.invokeMethod(
                  "highlight",
                  withArguments: [code, ["language": language, "ignoreIllegals": true]],
              )
        else { return nil }
        return result.objectForKeyedSubscript("value")?.toString()
    }

    /// Fence tag → hljs language id (the common bundle's ids; aliases
    /// users habitually type). Unknown tags fall back to plain text.
    private static func normalizedLanguage(_ tag: String?) -> String? {
        guard let tag else { return nil }
        let lower = tag.lowercased()
        switch lower {
        case "js", "jsx", "mjs", "cjs": return "javascript"
        case "ts", "tsx": return "typescript"
        case "py", "python3": return "python"
        case "sh", "shell", "zsh", "console": return "bash"
        case "c++", "cc", "cxx", "hpp", "hh": return "cpp"
        case "h": return "c"
        case "objc", "objective-c": return "objectivec"
        case "golang": return "go"
        case "rs": return "rust"
        case "rb": return "ruby"
        case "yml": return "yaml"
        case "md": return "markdown"
        case "kt": return "kotlin"
        case "cs": return "csharp"
        case "toml": return "ini"
        case "plain", "text", "txt", "plaintext": return nil
        default: return lower
        }
    }

    /// The unhighlighted rendering: mono + fg, same as before the
    /// highlighter existed.
    static func plain(_ code: String) -> AttributedString {
        AttributedString(NSAttributedString(string: code, attributes: [
            .font: NSFont.monospacedSystemFont(ofSize: Theme.textSm, weight: .regular),
            .foregroundColor: Palette.fg,
        ]))
    }
}

// MARK: - hljs HTML → NSAttributedString

/// hljs's output dialect is fixed: text nodes (with five escaped
/// entities) wrapped in `<span class="…">` — no other tags, no
/// attributes besides class. A stack walk suffices.
private enum HLJSHTMLConverter {
    private struct Style {
        var color: NSColor?
        var italic = false
        var bold = false
    }

    static func convert(_ html: String) -> NSAttributedString {
        let out = NSMutableAttributedString()
        var stack: [Style] = []
        var rest = Substring(html)

        while let open = rest.firstIndex(of: "<") {
            append(rest[..<open], style: stack.last ?? Style(), to: out)
            let afterOpen = rest[rest.index(after: open)...]
            guard let close = afterOpen.firstIndex(of: ">") else {
                append(afterOpen, style: stack.last ?? Style(), to: out)
                return out
            }
            let tag = afterOpen[..<close]
            if tag.hasPrefix("/") {
                if !stack.isEmpty {
                    stack.removeLast()
                }
            } else {
                stack.append(style(forTag: tag))
            }
            rest = afterOpen[afterOpen.index(after: close)...]
        }
        append(rest, style: stack.last ?? Style(), to: out)
        return out
    }

    private static func style(forTag tag: Substring) -> Style {
        guard let classStart = tag.range(of: "class=\"")?.upperBound,
              let classEnd = tag[classStart...].firstIndex(of: "\"")
        else { return Style() }
        let classes = tag[classStart ..< classEnd].split(separator: " ")
        var style = Style()
        // hljs v11 emits e.g. class="hljs-title class_": the CSS in
        // code.css styles .hljs-title.class_ more specifically than
        // .hljs-title, so the sub-scope tokens win the first pass.
        for name in classes {
            switch name {
            case "class_": style.color = Palette.warning
            case "function_": style.color = Palette.primary
            default: break
            }
        }
        for name in classes where style.color == nil {
            switch name {
            case "hljs-comment", "hljs-quote":
                style.color = Palette.muted
                style.italic = true
            case "hljs-keyword", "hljs-selector-tag", "hljs-meta":
                style.color = Palette.purple
            case "hljs-string", "hljs-regexp":
                style.color = Palette.success
            case "hljs-number", "hljs-literal":
                style.color = Palette.highlight
            case "hljs-title", "hljs-section":
                style.color = Palette.primary
            case "hljs-type":
                style.color = Palette.warning
            case "hljs-built_in", "hljs-attr", "hljs-attribute",
                 "hljs-variable", "hljs-template-variable":
                style.color = Palette.info
            case "hljs-name", "hljs-selector-id", "hljs-selector-class":
                style.color = Palette.error
            case "hljs-symbol", "hljs-bullet", "hljs-link":
                style.color = Palette.info
            default:
                break
            }
        }
        for name in classes {
            switch name {
            case "hljs-emphasis": style.italic = true
            case "hljs-strong": style.bold = true
            default: break
            }
        }
        return style
    }

    private static func append(_ raw: Substring, style: Style, to out: NSMutableAttributedString) {
        guard !raw.isEmpty else { return }
        let base = NSFont.monospacedSystemFont(
            ofSize: Theme.textSm,
            weight: style.bold ? .bold : .regular,
        )
        let font = style.italic
            ? NSFontManager.shared.convert(base, toHaveTrait: .italicFontMask)
            : base
        out.append(NSAttributedString(string: decodeEntities(raw), attributes: [
            .font: font,
            .foregroundColor: style.color ?? Palette.fg,
        ]))
    }

    /// hljs escapes exactly five entities; decode in ONE pass so a
    /// literal "&lt;" in the source (escaped as "&amp;lt;") survives.
    private static func decodeEntities(_ text: Substring) -> String {
        var out = ""
        out.reserveCapacity(text.count)
        var i = text.startIndex
        while i < text.endIndex {
            guard text[i] == "&", let semi = text[i...].firstIndex(of: ";") else {
                out.append(text[i])
                i = text.index(after: i)
                continue
            }
            switch text[text.index(after: i) ..< semi] {
            case "amp": out.append("&")
            case "lt": out.append("<")
            case "gt": out.append(">")
            case "quot": out.append("\"")
            case "#x27", "#39": out.append("'")
            default: out.append(contentsOf: text[i ... semi])
            }
            i = text.index(after: semi)
        }
        return out
    }
}

// MARK: - Palette

/// tokens.css palette as appearance-adaptive NSColors — the hex pairs
/// mirror Theme.swift (keep in sync); SwiftUI's Color can't be stored
/// inside an NSAttributedString.
private enum Palette {
    static let fg = adaptive(dark: 0xD3C6AA, light: 0x5C6A72)
    static let muted = adaptive(dark: 0x9DA9A0, light: 0x5C6E5E)
    static let primary = adaptive(dark: 0x7FBBB3, light: 0x2273A8)
    static let success = adaptive(dark: 0xA7C080, light: 0x8DA101)
    static let info = adaptive(dark: 0x83C092, light: 0x35A77C)
    static let warning = adaptive(dark: 0xDBBC7F, light: 0xDFA000)
    static let error = adaptive(dark: 0xE67E80, light: 0xF85552)
    static let highlight = adaptive(dark: 0xE69875, light: 0xF57D26)
    static let purple = adaptive(dark: 0xD699B6, light: 0xDF69BA)

    private static func adaptive(dark: UInt32, light: UInt32) -> NSColor {
        NSColor(name: nil) { appearance in
            let hex = appearance.bestMatch(from: [.darkAqua, .aqua]) == .darkAqua ? dark : light
            return NSColor(
                red: CGFloat((hex >> 16) & 0xFF) / 255,
                green: CGFloat((hex >> 8) & 0xFF) / 255,
                blue: CGFloat(hex & 0xFF) / 255,
                alpha: 1,
            )
        }
    }
}
