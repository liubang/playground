import java.net.URL;

/**
 * {Insert class description here}
 *
 * @author <a href="mailto:liubang@staff.weibo.com">liubang</a>
 * @version : {Version} $ : 2019-04-08 18:01 $
 * @see
 */
public class Demo {

    public static void main(String[] args) throws Exception {
        URL url = new URL("http://", "localhost", 80, null);
        System.out.println(url.toString());
    }
}
