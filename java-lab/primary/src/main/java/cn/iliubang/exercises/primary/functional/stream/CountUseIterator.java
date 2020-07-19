package cn.iliubang.exercises.primary.functional.stream;

import java.util.Iterator;
import java.util.List;

import static cn.iliubang.exercises.primary.functional.stream.ArtistList.Artist;

/**
 * {Insert class description here}
 *
 * @author <a href="mailto:liubang@staff.weibo.com">liubang</a>
 * @version $Revision: {Version} $ $Date: 2018/9/27 15:23 $
 * @see
 */
public class CountUseIterator {

    public static void main(String[] args) {
        List<Artist> allArtists = ArtistList.buildArtistList();
        int count = 0;
        Iterator<Artist> iterator = allArtists.iterator();
        while (iterator.hasNext()) {
            Artist artist = iterator.next();
            if (artist.isFrom("London")) {
                count++;
            }
        }
        System.out.println(count);
    }
}
