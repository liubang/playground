package cn.iliubang.exercises.dp.factory.abstractfactory;

public class LowTyre implements Tyre {
    @Override
    public void revolve() {
        System.out.println("磨损快");
    }
}
