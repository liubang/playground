package cn.iliubang.exercises.primary.functional.stream;

import java.util.List;

import static cn.iliubang.exercises.primary.functional.stream.ArtistList.Artist;
import static cn.iliubang.exercises.primary.functional.stream.ArtistList.buildArtistList;

/**
 * {Insert class description here}
 *
 * @author <a href="mailto:liubang@staff.weibo.com">liubang</a>
 * @version $Revision: {Version} $ $Date: 2018/9/27 15:25 $
 * @see
 */
public class CountUseStream {
    public static void main(String[] args) {
        List<Artist> allArtists = buildArtistList();
        long count = allArtists.stream().filter(artist -> artist.isFrom("London")).count();

        System.out.println(count);

        /*
         * 惰性求值
         * 仅仅使用filter这样刻画流的函数并不会执行实际性的工作
         * 只有在使用的类似于count这种的终止操作的流，才会触发惰性求值方法的执行
         */
        allArtists.stream().filter(artist -> {
            System.out.println(artist.getName());
            return artist.isFrom("London");
        });

        System.out.println("=====count=====");
        allArtists.stream().filter(artist -> {
            System.out.println(artist.getName());
            return artist.isFrom("London");
        }).count();
    }
}
