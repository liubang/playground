package cn.iliubang.exercises.dp.factory.abstractfactory;

public class Client01 {
    public static void main(String[] args) {
        CarFactory factory = new LuxuryCarFactory();
        Engine e = factory.createEngine();
        e.run();
        e.start();
    }
}
