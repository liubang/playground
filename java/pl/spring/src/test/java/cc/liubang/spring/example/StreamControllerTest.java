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

package cc.liubang.spring.example;

import org.junit.jupiter.api.Test;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.boot.test.autoconfigure.web.servlet.AutoConfigureMockMvc;
import org.springframework.boot.test.context.SpringBootTest;
import org.springframework.test.web.servlet.MockMvc;
import org.springframework.test.web.servlet.MvcResult;

import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;
import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.asyncDispatch;
import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.get;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.content;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.request;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.status;

@SpringBootTest
@AutoConfigureMockMvc
class StreamControllerTest {

    @Autowired
    private MockMvc mockMvc;

    @Test
    void streamEvents_returnsAllEvents() throws Exception {
        MvcResult mvcResult = mockMvc.perform(get("/api/stream/events/3"))
            .andExpect(request().asyncStarted())
            .andReturn();

        // Wait for async to complete
        mvcResult.getAsyncResult(5000);

        String body = mockMvc.perform(asyncDispatch(mvcResult))
            .andExpect(status().isOk())
            .andReturn()
            .getResponse()
            .getContentAsString();

        assertTrue(body.contains("event-1"));
        assertTrue(body.contains("event-2"));
        assertTrue(body.contains("event-3"));
    }

    @Test
    void streamChat_returnsTokensAndDone() throws Exception {
        MvcResult mvcResult = mockMvc.perform(get("/api/stream/chat"))
            .andExpect(request().asyncStarted())
            .andReturn();

        mvcResult.getAsyncResult(5000);

        String body = mockMvc.perform(asyncDispatch(mvcResult))
            .andExpect(status().isOk())
            .andReturn()
            .getResponse()
            .getContentAsString();

        // Verify tokens are present
        assertTrue(body.contains("Hello"));
        assertTrue(body.contains("Spring"));
        assertTrue(body.contains("SSE"));
        // Verify done signal
        assertTrue(body.contains("[DONE]"));
    }

    @Test
    void streamEvents_singleEvent() throws Exception {
        MvcResult mvcResult = mockMvc.perform(get("/api/stream/events/1"))
            .andExpect(request().asyncStarted())
            .andReturn();

        mvcResult.getAsyncResult(5000);

        String body = mockMvc.perform(asyncDispatch(mvcResult))
            .andExpect(status().isOk())
            .andReturn()
            .getResponse()
            .getContentAsString();

        assertTrue(body.contains("event-1"));
        // Should not contain event-2
        assertFalse(body.contains("event-2"));
    }
}
