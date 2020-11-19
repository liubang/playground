package cn.iliubang.exercises.primary.functional.stream;

import java.util.List;

import static cn.iliubang.exercises.primary.functional.stream.ArtistList.Artist;
import static cn.iliubang.exercises.primary.functional.stream.ArtistList.buildArtistList;

/**
 * {Insert class description here}
 *
 * @author <a href="mailto:liubang@staff.weibo.com">liubang</a>
 * @version $Revision: {Version} $ $Date: 2018/9/27 15:18 $
 * @see
 */
public class CountUseFor {

    public static void main(String[] args) {
        List<Artist> allArtists = buildArtistList();

        int count = 0;

        for (Artist artist : allArtists) {
            if (artist.isFrom("London")) {
                count++;
            }
        }

        System.out.println(count);
    }
}
