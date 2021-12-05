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

final class RouterRule
{
    private $method;

    private $uri;

    private $compiledUri;

    private $class;

    private $classMethod;

    private $paramsMap;

    /**
     * RouterRule constructor.
     * @param $method
     * @param $uri
     * @param $class
     * @param $classMethod
     */
    public function __construct($method, $uri, $class, $classMethod)
    {
        $this->method = $method;
        $this->uri = $uri;
        $this->class = $class;
        $this->classMethod = $classMethod;
    }

    /**
     * @return mixed
     */
    public function getMethod()
    {
        return $this->method;
    }

    /**
     * @return mixed
     */
    public function getUri()
    {
        return $this->uri;
    }

    /**
     * @return mixed
     */
    public function getClass()
    {
        return $this->class;
    }

    /**
     * @return mixed
     */
    public function getClassMethod()
    {
        return $this->classMethod;
    }

    /**
     * @return mixed
     */
    public function getCompiledUri()
    {
        return $this->compiledUri;
    }

    /**
     * @param mixed $compiledUri
     */
    public function setCompiledUri($compiledUri)
    {
        $this->compiledUri = $compiledUri;
    }

    /**
     * @return mixed
     */
    public function getParamsMap()
    {
        return $this->paramsMap;
    }

    /**
     * @param mixed $paramsMap
     */
    public function setParamsMap($paramsMap)
    {
        $this->paramsMap = $paramsMap;
    }
}
