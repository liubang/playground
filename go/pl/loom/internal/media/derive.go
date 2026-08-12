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

// Authors: liubang (it.liubang@gmail.com)
// Created: 2026/08/12

package media

import (
	"bytes"
	"encoding/base64"
	"image"
	"image/jpeg"
	"image/png"

	xdraw "golang.org/x/image/draw"

	"github.com/liubang/playground/go/pl/loom/internal/domain"
)

// downscale proportionally shrinks img so both dimensions fit maxDim,
// returning img unchanged when it already fits. CatmullRom is a good
// quality/speed trade-off for photographic content (codex uses the
// equivalent Triangle filter; the difference is invisible at this scale).
func downscale(img image.Image, maxDim int) image.Image {
	bounds := img.Bounds()
	w, h := bounds.Dx(), bounds.Dy()
	if w <= maxDim && h <= maxDim {
		return img
	}
	var nw, nh int
	if w >= h {
		nw = maxDim
		nh = max(1, h*maxDim/w)
	} else {
		nh = maxDim
		nw = max(1, w*maxDim/h)
	}
	dst := image.NewNRGBA(image.Rect(0, 0, nw, nh))
	xdraw.CatmullRom.Scale(dst, dst.Bounds(), img, bounds, xdraw.Over, nil)
	return dst
}

// isOpaque reports whether img has no transparent pixels. Opaque is not on
// the image.Image interface but is implemented by every concrete type the
// supported decoders (and downscale) produce; unknown types conservatively
// report non-opaque so alpha is never silently flattened to jpeg.
func isOpaque(img image.Image) bool {
	if o, ok := img.(interface{ Opaque() bool }); ok {
		return o.Opaque()
	}
	return false
}

// encode re-encodes img for the wire: jpeg for opaque images (smaller,
// matches the photographic majority), png when an alpha channel is present
// (jpeg cannot represent transparency).
func encode(img image.Image) (domain.ImageContent, error) {
	var buf bytes.Buffer
	if isOpaque(img) {
		if err := jpeg.Encode(&buf, img, &jpeg.Options{Quality: jpegQuality}); err != nil {
			return domain.ImageContent{}, domain.NewError(domain.ErrInternal, "media: encode jpeg", domain.WithCause(err))
		}
		return domain.ImageContent{MediaType: "image/jpeg", Data: base64.StdEncoding.EncodeToString(buf.Bytes())}, nil
	}
	if err := png.Encode(&buf, img); err != nil {
		return domain.ImageContent{}, domain.NewError(domain.ErrInternal, "media: encode png", domain.WithCause(err))
	}
	return domain.ImageContent{MediaType: "image/png", Data: base64.StdEncoding.EncodeToString(buf.Bytes())}, nil
}
