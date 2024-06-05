package cn.iliubang.exercises.primary.functional.stream;

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

    // @Builder
    public static class Artist {
        private int id;
        private String name;
        private String from;

        public Artist() {
        }

        public Artist(int id, String name, String from) {
            this.id = id;
            this.name = name;
            this.from = from;
        }

        public boolean isFrom(String f) {
            return from.equals(f);
        }

        public int getId() {
            return id;
        }

        public void setId(int id) {
            this.id = id;
        }

        public String getName() {
            return name;
        }

        public void setName(String name) {
            this.name = name;
        }

        public String getFrom() {
            return from;
        }

        public void setFrom(String from) {
            this.from = from;
        }
    }

    public static List<Artist> buildArtistList() {
        List<Artist> artists = new ArrayList<>();
        artists.add(new Artist(1, "liubang1", "Beijing"));
        artists.add(new Artist(2, "liubang2", "London"));
        artists.add(new Artist(3, "liubang3", "Shanghai"));
        artists.add(new Artist(4, "liubang4", "London"));
        return artists;
    }
}
