package cn.iliubang.exercises.test;

import org.junit.Assert;
import org.junit.Test;
import org.mockito.ArgumentCaptor;

import java.util.*;

import static org.mockito.Mockito.*;

/**
 * {Insert class description here}
 *
 * @author <a href="mailto:liubang@staff.weibo.com">liubang</a>
 * @version $Revision: {Version} $ $Date: 2019-02-15 15:30 $
 */
public class TestMockito {

    @Test
    public void createMockObject() {
        // 使用mock静态方法创建Mock对象
        List mockedList = mock(List.class);
        Assert.assertTrue(mockedList instanceof List);
        ArrayList mockedArrayList = mock(ArrayList.class);

        Assert.assertTrue(mockedArrayList instanceof List);
        Assert.assertTrue(mockedArrayList instanceof ArrayList);
    }

    @Test
    public void configMockObject() {
        List mockedList = mock(List.class);

        // 我们定制了当调用mockedList.add("one")时，返回true
        when(mockedList.add("one")).thenReturn(true);
        // 当调用mockedList.size()时，返回1
        when(mockedList.size()).thenReturn(1);

        Assert.assertTrue(mockedList.add("one"));
        Assert.assertFalse(mockedList.add("One"));
        Assert.assertEquals(1, mockedList.size());

        Iterator i = mock(Iterator.class);
        when(i.next()).thenReturn("hello,").thenReturn("Mockito!");

        String result = i.next() + " " + i.next();

        Assert.assertEquals("hello, Mockito!", result);
    }

    @Test(expected = NoSuchElementException.class)
    public void testForIOException() throws Exception {
        Iterator i = mock(Iterator.class);
        when(i.next()).thenReturn("hello,").thenReturn("Mockito!");
        String result = i.next() + " " + i.next();
        Assert.assertEquals("hello, Mockito!", result);

        // doThrow(ExceptionX).when(x).methodCall, 它的含义是:
        // 当调用了 x.methodCall 方法后, 抛出异常 ExceptionX.
        doThrow(new NoSuchElementException()).when(i).next();
        i.next();
    }

    @Test
    public void testVerify() {
        List mockedList = mock(List.class);
        mockedList.add("one");
        mockedList.add("two");
        mockedList.add("three times");
        mockedList.add("three times");
        mockedList.add("three times");

        when(mockedList.size()).thenReturn(5);
        Assert.assertEquals(5, mockedList.size());
        // 第一句校验 mockedList.add("one") 至少被调用了 1 次(atLeastOnce)
        // 第二句校验 mockedList.add("two") 被调用了 1 次(times(1))
        // 第三句校验 mockedList.add("three times") 被调用了 3 次(times(3))
        // 第四句校验 mockedList.isEmpty() 从未被调用(never)
        verify(mockedList, atLeastOnce()).add("one");
        verify(mockedList, times(1)).add("two");
        verify(mockedList, times(3)).add("three times");
        verify(mockedList, never()).isEmpty();
    }

    @Test
    public void testSpy() {
        List list = new LinkedList();
        List spy = spy(list);

        // 对spy.size()进行定制
        when(spy.size()).thenReturn(5);

        spy.add("one");
        spy.add("two");

        // 因为我们没有对git(0), get(1)方法进行定制
        // 因此这些调用其实是调用的真实对象的方法
        Assert.assertEquals(spy.get(0), "one");
        Assert.assertEquals(spy.get(1), "two");

        Assert.assertEquals(spy.size(), 5);
    }

    @Test
    public void testCaptureArgument() {
        List<String> list = Arrays.asList("1", "2");
        List mockedList = mock(List.class);
        ArgumentCaptor<List> argumentCaptor = ArgumentCaptor.forClass(List.class);
        mockedList.addAll(list);

        verify(mockedList).addAll(argumentCaptor.capture());
        Assert.assertEquals(2, argumentCaptor.getValue().size());
        Assert.assertEquals(list, argumentCaptor.getValue());
    }
}
