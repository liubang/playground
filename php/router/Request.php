<?php

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
