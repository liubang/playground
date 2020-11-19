<?php

namespace router;

class Router
{

    private $routers = [];

    private $maxIndex = 1;

    private $currChunk = 0;

    private $currNum = 1;

    const preg = '~(?:@(.*?)\:)~x';

    private function getChunkSize()
    {
        return 10;
    }

    /**
     * @return RouterRule[]
     */
    public function getRouters()
    {
        return $this->routers;
    }


    public function addRoute(RouterRule $rule)
    {
        if ($this->currNum > $this->getChunkSize()) {
            $this->maxIndex = 1;
            $this->currChunk++;
            $this->currNum = 1;
        }
        $this->currNum++;
        $uri = $rule->getUri();
        $compiledUri = $uri;
        $c = 1;
        if (\preg_match_all(self::preg, $uri, $matches)) {
            if (isset($matches[1])) {
                $c = count($matches[1]) + 1;
                $compiledUri = \preg_replace(self::preg, '', $uri);
                $rule->setParamsMap($matches[1]);
            }
        }

        $this->maxIndex = \max($this->maxIndex, $c);
        $compiledUri .= \str_repeat('()', $this->maxIndex - $c);

        $rule->setCompiledUri($compiledUri);
        $this->routers[$this->currChunk][$this->maxIndex] = $rule;
        $this->maxIndex++;
    }

    public function get($uri, $class, $classMethod)
    {
        $rule = new RouterRule(Request::GET, $uri, $class, $classMethod);
        $this->addRoute($rule);
    }

    public function post($uri, $class, $classMethod)
    {
        $rule = new RouterRule(Request::POST, $uri, $class, $classMethod);
        $this->addRoute($rule);
    }

    public function put($uri, $class, $classMethod)
    {
        $rule = new RouterRule(Request::PUT, $uri, $class, $classMethod);
        $this->addRoute($rule);
    }

    public function delete($uri, $class, $classMethod)
    {
        $rule = new RouterRule(Request::DELETE, $uri, $class, $classMethod);
        $this->addRoute($rule);
    }

    public function dump() {
        print_r($this->routers);
    }
}
