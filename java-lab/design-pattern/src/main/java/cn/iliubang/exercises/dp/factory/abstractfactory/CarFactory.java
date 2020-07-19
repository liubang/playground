package cn.iliubang.exercises.dp.factory.abstractfactory;

public interface CarFactory {
    Engine createEngine();

    Seat createSeat();

    Tyre createTyre();
}
