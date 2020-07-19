<?php

namespace router;

class Dispatcher
{

    /**
     * @var Router
     */
    private $router;

    private $routerRule;

    public function __construct()
    {
        $this->router = new Router();
    }

    /**
     * @return mixed
     */
    public function getRouterRule()
    {
        return $this->routerRule;
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
                $this->routerRule = $rule;
                break;
            }
        }
    }

}