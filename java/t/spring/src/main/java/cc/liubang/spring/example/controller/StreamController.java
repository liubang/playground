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
// Created: 2026/05/16 21:43

package cc.liubang.spring.example.controller;

import org.springframework.http.MediaType;
import org.springframework.web.bind.annotation.GetMapping;
import org.springframework.web.bind.annotation.PathVariable;
import org.springframework.web.bind.annotation.RequestMapping;
import org.springframework.web.bind.annotation.RestController;
import org.springframework.web.servlet.mvc.method.annotation.SseEmitter;

import java.io.IOException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

@RestController
@RequestMapping("/api/stream")
public class StreamController {

    private final ExecutorService executor = Executors.newCachedThreadPool();

    /**
     * SSE endpoint that streams a sequence of numbered events.
     * Each event is sent with a small delay to simulate real-time data push.
     */
    @GetMapping(value = "/events/{count}", produces = MediaType.TEXT_EVENT_STREAM_VALUE)
    public SseEmitter streamEvents(@PathVariable int count) {
        SseEmitter emitter = new SseEmitter(30_000L);

        executor.execute(() -> {
            try {
                for (int i = 1; i <= count; i++) {
                    emitter.send(SseEmitter.event()
                        .id(String.valueOf(i))
                        .name("message")
                        .data("event-" + i));
                    Thread.sleep(50);
                }
                emitter.complete();
            } catch (IOException | InterruptedException e) {
                emitter.completeWithError(e);
            }
        });

        return emitter;
    }

    /**
     * SSE endpoint that streams tokens of a sentence word by word,
     * simulating an LLM-style streaming response.
     */
    @GetMapping(value = "/chat", produces = MediaType.TEXT_EVENT_STREAM_VALUE)
    public SseEmitter streamChat() {
        SseEmitter emitter = new SseEmitter(30_000L);
        String[] tokens = {"Hello", " ", "from", " ", "Spring", " ", "Boot", " ", "SSE", "!"};

        executor.execute(() -> {
            try {
                for (int i = 0; i < tokens.length; i++) {
                    emitter.send(SseEmitter.event()
                        .id(String.valueOf(i))
                        .name("token")
                        .data(tokens[i]));
                    Thread.sleep(30);
                }
                emitter.send(SseEmitter.event().name("done").data("[DONE]"));
                emitter.complete();
            } catch (IOException | InterruptedException e) {
                emitter.completeWithError(e);
            }
        });

        return emitter;
    }
}
