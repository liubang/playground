package cn.iliubang.exercises.dp.factory.abstractfactory;

public class LowSeat implements Seat {
    @Override
    public void message() {
        System.out.println("很硬，不舒服");
    }
}
