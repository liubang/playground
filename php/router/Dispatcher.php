<?php

namespace router;

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
            throw new \Exception("404 Not Found.");
        }

        $clazz = $routerRule->getClass();
        if (!class_exists($clazz)) {
            throw new \Exception("{$clazz} not exists.");
        }
        $class = new $clazz();
        return $routerRule;
    }
}
