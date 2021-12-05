<?php

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
