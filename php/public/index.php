<?php

ini_set('display_errors', 1);
error_reporting(E_ALL);

define('ROOT', realpath(__DIR__ . '/../') . '/');

spl_autoload_register(function ($class) {
    $classFile = ROOT . str_replace('\\', '/', $class) . '.php';
    if (is_file($classFile)) {
        include $classFile;
    } else {
        throw new \Exception("class " . $class . " not found.");
    }
}, true);


$dispatcher = new \router\Dispatcher();
$router = $dispatcher->getRouter();

$start = microtime(true);

for ($i = 0; $i < 100; $i++) {
    $router->get('/home' . $i . '/@name:([a-z]+)/@id:(\d+)', 'Test', 'home');
}
$router->get('/home/@name:([a-z]+)/@age:(\d+)/@id:(\d+)', 'Test', 'home');
//$router->dump();
$dispatcher->dispatch(new \router\Request('/home/liubang/25/88888', \router\Request::GET));

echo microtime(true) - $start . PHP_EOL;
