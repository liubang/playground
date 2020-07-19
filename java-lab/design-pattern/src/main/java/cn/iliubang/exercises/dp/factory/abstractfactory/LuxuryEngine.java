package cn.iliubang.exercises.dp.factory.abstractfactory;

public class LuxuryEngine implements Engine {

    @Override
    public void run() {
        System.out.println("省油");
    }

    @Override
    public void start() {
        System.out.println("启动快");
    }
}
