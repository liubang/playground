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

class Request
{
    const GET = 1;

    const POST = 2;

    const PUT = 3;

    const DELETE = 4;

    private $params;

    private $uri;

    private $requestMethod;

    /**
     * Request constructor.
     * @param $uri
     * @param $requestMethod
     */
    public function __construct($uri, $requestMethod)
    {
        $this->uri = $uri;
        $this->requestMethod = $requestMethod;
    }


    /**
     * @param mixed $params
     */
    public function setParams($params)
    {
        $this->params = $params;
    }

    /**
     * @return mixed
     */
    public function getParams()
    {
        return $this->params;
    }

    /**
     * @return mixed
     */
    public function getUri()
    {
        return $this->uri;
    }

    /**
     * @param mixed $uri
     */
    public function setUri($uri)
    {
        $this->uri = $uri;
    }

    /**
     * @return mixed
     */
    public function getRequestMethod()
    {
        return $this->requestMethod;
    }

    /**
     * @param mixed $requestMethod
     */
    public function setRequestMethod($requestMethod)
    {
        $this->requestMethod = $requestMethod;
    }
}

