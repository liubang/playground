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

import cc.liubang.spring.example.dto.CreateUserRequest;
import cc.liubang.spring.example.dto.UserVO;
import cc.liubang.spring.example.exception.BusinessException;
import cc.liubang.spring.example.exception.ResourceNotFoundException;
import cc.liubang.spring.example.service.UserService;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

class UserServiceTest {

    private UserService userService;

    @BeforeEach
    void setUp() {
        userService = new UserService();
    }

    @Test
    void createUser_success() {
        CreateUserRequest request = new CreateUserRequest("Alice", "alice@example.com", 25);
        UserVO user = userService.createUser(request);

        assertNotNull(user.getId());
        assertEquals("Alice", user.getName());
        assertEquals("alice@example.com", user.getEmail());
        assertEquals(25, user.getAge());
    }

    @Test
    void createUser_duplicateEmail_throwsBusinessException() {
        CreateUserRequest request = new CreateUserRequest("Alice", "alice@example.com", 25);
        userService.createUser(request);

        BusinessException ex = assertThrows(BusinessException.class,
            () -> userService.createUser(new CreateUserRequest("Bob", "alice@example.com", 30)));
        assertEquals(409, ex.getCode());
        assertTrue(ex.getMessage().contains("alice@example.com"));
    }

    @Test
    void getUser_exists() {
        CreateUserRequest request = new CreateUserRequest("Alice", "alice@example.com", 25);
        UserVO created = userService.createUser(request);

        UserVO found = userService.getUser(created.getId());
        assertEquals(created.getId(), found.getId());
        assertEquals("Alice", found.getName());
    }

    @Test
    void getUser_notFound_throwsResourceNotFoundException() {
        assertThrows(ResourceNotFoundException.class,
            () -> userService.getUser("nonexistent"));
    }

    @Test
    void listUsers_empty() {
        assertTrue(userService.listUsers().isEmpty());
    }

    @Test
    void listUsers_afterCreation() {
        userService.createUser(new CreateUserRequest("Alice", "a@example.com", 25));
        userService.createUser(new CreateUserRequest("Bob", "b@example.com", 30));

        assertEquals(2, userService.listUsers().size());
    }

    @Test
    void deleteUser_success() {
        UserVO user = userService.createUser(
            new CreateUserRequest("Alice", "alice@example.com", 25));
        userService.deleteUser(user.getId());

        assertThrows(ResourceNotFoundException.class,
            () -> userService.getUser(user.getId()));
    }

    @Test
    void deleteUser_notFound_throwsResourceNotFoundException() {
        assertThrows(ResourceNotFoundException.class,
            () -> userService.deleteUser("nonexistent"));
    }

    @Test
    void createMultipleUsers_uniqueIds() {
        UserVO u1 = userService.createUser(new CreateUserRequest("A", "a@x.com", 20));
        UserVO u2 = userService.createUser(new CreateUserRequest("B", "b@x.com", 21));
        assertFalse(u1.getId().equals(u2.getId()));
    }
}
