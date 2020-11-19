package cn.iliubang.exercises.dp.factory.abstractfactory;

public class LowEngine implements Engine {

    @Override
    public void run() {
        System.out.println("费油");
    }

    @Override
    public void start() {
        System.out.println("慢");
    }
}
