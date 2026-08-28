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

#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>

// Recursively finds the WKWebView(s) in wails' content tree and retint them.
// The webview's underPageBackgroundColor (default white) is what actually
// shows during a live resize — setting just the NSWindow's backgroundColor
// leaves the webview's own white visible on top of it.
static int loomTintViewTree(NSView *root, NSColor *c) {
  if (!root) {
    return 0;
  }
  int tinted = 0;
  if ([root isKindOfClass:[WKWebView class]]) {
    WKWebView *wv = (WKWebView *)root;
    wv.underPageBackgroundColor = c;
    if (wv.layer) {
      wv.layer.backgroundColor = c.CGColor;
    }
    tinted++;
  }
  for (NSView *sub in root.subviews) {
    tinted += loomTintViewTree(sub, c);
  }
  return tinted;
}

static void loomTintNow(float r, float g, float b, const char *phase) {
  NSColor *c = [NSColor colorWithCalibratedRed:r green:g blue:b alpha:1.0];
  NSArray<NSWindow *> *windows = [NSApplication sharedApplication].windows;
  int tinted = 0;
  for (NSWindow *w in windows) {
    [w setBackgroundColor:c];
    tinted += loomTintViewTree([w contentView], c);
  }
  fprintf(stderr, "loom-desktop: window-bg %s — windows=%lu webviewsTinted=%d\n", phase,
          (unsigned long)windows.count, tinted);
}

// OnStartup races wails' window/webview setup (the hook fires on a goroutine
// while Run() drives the main loop), so a single shot can land before any
// NSWindow is registered with NSApp. Retry on the main queue with short
// delays until the tint actually reaches at least one webview.
void loomSetWindowBackgroundColor(float r, float g, float b) {
  static const double delays[] = {0, 0.2, 1.0};
  for (int i = 0; i < 3; i++) {
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(delays[i] * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
                     loomTintNow(r, g, b, i == 0 ? "immediate" : (i == 1 ? "+200ms" : "+1s"));
                   });
  }
}
