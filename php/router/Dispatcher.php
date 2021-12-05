<?php

// Copyright (c) 2020 The Authors. All rights reserved.
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
// Created: 2020/07/20 00:32

namespace router;

use FFI\Exception;

class Dispatcher
{

    private $router;

    public function __construct()
    {
        $this->router = new Router();
    }

    /**
     * @return Router
     */
    public function getRouter()
    {
        return $this->router;
    }

    /**
     * @param Request $request
     */
    public function dispatch(Request $request)
    {
        $uri = $request->getUri();
        $requestMethod = $request->getRequestMethod();
        $routerRule = null;
        foreach ($this->router->getRouters() as $chunk) {
            $preg = '';
            foreach ($chunk as $item) {
                if ($item->getMethod() == $requestMethod) {
                    $preg .= '|' . $item->getCompiledUri();
                }
            }
            $preg = \sprintf('~^(?%s)$~x', $preg);
            if (\preg_match($preg, $uri, $matches)) {
                $index = count($matches);
                $rule = $chunk[$index];
                $paramsMap = $rule->getParamsMap();
                $params = [];
                if (!empty($paramsMap)) {
                    $i = 1;
                    foreach ($paramsMap as $name) {
                        $params[$name] = $matches[$i++];
                    }
                }
                $request->setParams($params);
                $routerRule = $rule;
                break;
            }
        }

        if (is_null($routerRule)) {
            throw new Exception("404 Not Found.");
        }

        $clazz = $routerRule->getClass();
        if (!class_exists($clazz)) {
            throw new Exception("{$clazz} not exists.");
        }
        $class = new $clazz();
        return $routerRule;
    }
}
