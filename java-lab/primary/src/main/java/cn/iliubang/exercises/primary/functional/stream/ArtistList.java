package cn.iliubang.exercises.primary.functional.stream;

import lombok.Builder;
import lombok.Data;

import java.util.ArrayList;
import java.util.List;

/**
 * {Insert class description here}
 *
 * @author <a href="mailto:liubang@staff.weibo.com">liubang</a>
 * @version $Revision: {Version} $ $Date: 2018/9/27 15:14 $
 * @see
 */

public class ArtistList {

    @Data
    @Builder
    public static class Artist {
        private int id;
        private String name;
        private String from;

        public boolean isFrom(String f) {
            return from.equals(f);
        }
    }

    public static List<Artist> buildArtistList() {
        List<Artist> artists = new ArrayList<>();
        artists.add(Artist.builder().id(1).name("liubang1").from("Beijing").build());
        artists.add(Artist.builder().id(2).name("liubang2").from("London").build());
        artists.add(Artist.builder().id(3).name("liubang3").from("Shanghai").build());
        artists.add(Artist.builder().id(4).name("liubang4").from("London").build());
        return artists;
    }
}
