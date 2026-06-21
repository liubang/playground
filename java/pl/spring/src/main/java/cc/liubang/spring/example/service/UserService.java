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

package cc.liubang.spring.example.service;

import cc.liubang.spring.example.dto.CreateUserRequest;
import cc.liubang.spring.example.dto.UserVO;
import cc.liubang.spring.example.exception.BusinessException;
import cc.liubang.spring.example.exception.ResourceNotFoundException;
import org.springframework.stereotype.Service;

import java.util.List;
import java.util.Map;
import java.util.UUID;
import java.util.concurrent.ConcurrentHashMap;

@Service
public class UserService {

    private final Map<String, UserVO> users = new ConcurrentHashMap<>();

    public UserVO createUser(CreateUserRequest request) {
        // Check for duplicate email
        boolean emailExists = users.values().stream()
            .anyMatch(u -> u.getEmail().equals(request.getEmail()));
        if (emailExists) {
            throw new BusinessException(409, "email already exists: " + request.getEmail());
        }

        String id = UUID.randomUUID().toString().substring(0, 8);
        UserVO user = new UserVO(id, request.getName(), request.getEmail(), request.getAge());
        users.put(id, user);
        return user;
    }

    public UserVO getUser(String id) {
        UserVO user = users.get(id);
        if (user == null) {
            throw new ResourceNotFoundException("User", id);
        }
        return user;
    }

    public List<UserVO> listUsers() {
        return List.copyOf(users.values());
    }

    public void deleteUser(String id) {
        if (users.remove(id) == null) {
            throw new ResourceNotFoundException("User", id);
        }
    }
}
