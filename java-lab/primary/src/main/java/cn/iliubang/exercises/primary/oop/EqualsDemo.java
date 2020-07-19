package cn.iliubang.exercises.primary.oop;

/**
 * @author liubang <it.liubang@gmail.com>
 * @version 1.0
 * @since 2018/1/15
 */
public class EqualsDemo {

    private String firstName;

    private String lastName;

    public EqualsDemo(String firstName, String lastName) {
        this.firstName = firstName;
        this.lastName = lastName;
    }

    @Override
    public int hashCode() {
        return firstName.hashCode();
    }

    @Override
    public boolean equals(Object obj) {
        if (obj instanceof EqualsDemo) {
            EqualsDemo equalsDemo = (EqualsDemo) obj;
            return equalsDemo.firstName.equals(firstName) && equalsDemo.lastName.equals(lastName);
        } else {
            return super.equals(obj);
        }
    }
}
