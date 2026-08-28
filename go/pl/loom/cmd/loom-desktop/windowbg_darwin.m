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

// Recursively finds the WKWebView in the wails content tree and retint it.
// The webview's underPageBackgroundColor (default white) is what actually
// shows during a live resize — setting just the NSWindow's backgroundColor
// leaves the webview's own white visible on top of it.
static void loomTintViewTree(NSView *root, NSColor *c) {
  if (!root) {
    return;
  }
  if ([root isKindOfClass:[WKWebView class]]) {
    WKWebView *wv = (WKWebView *)root;
    wv.underPageBackgroundColor = c;
    if (wv.layer) {
      wv.layer.backgroundColor = c.CGColor;
    }
  }
  for (NSView *sub in root.subviews) {
    loomTintViewTree(sub, c);
  }
}

void loomSetWindowBackgroundColor(float r, float g, float b) {
  dispatch_async(dispatch_get_main_queue(), ^{
    NSColor *c = [NSColor colorWithCalibratedRed:r green:g blue:b alpha:1.0];
    for (NSWindow *w in [NSApplication sharedApplication].windows) {
      [w setBackgroundColor:c];
      loomTintViewTree([w contentView], c);
    }
  });
}
