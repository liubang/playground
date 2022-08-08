package cn.iliubang.exercises.flink;

import lombok.AllArgsConstructor;
import lombok.Data;
import org.apache.flink.api.common.functions.FlatMapFunction;
import org.apache.flink.api.common.functions.ReduceFunction;
import org.apache.flink.streaming.api.datastream.DataStream;
import org.apache.flink.streaming.api.datastream.DataStreamSource;
import org.apache.flink.streaming.api.environment.StreamExecutionEnvironment;
import org.apache.flink.streaming.api.windowing.time.Time;

/**
 * 通过socket模拟产生单词数据
 * flink对数据进行统计计算
 * <p>
 * 需要实现每隔1秒对最近2秒内的数据进行汇总计算(滑动窗口计算)
 *
 * @author <a href="mailto:it.liubang@gmail.com">liubang</a>
 * @version : {Version} $ : 2019-08-04 21:24 $
 */
public class SocketWindowWordCountJava {

    public static void main(String[] args) throws Exception {
        String host = "localhost";
        int port = 8080;
        String delimiter = "\n";
        long maxRetry = 2;

        // 获取flink的运行环境
        StreamExecutionEnvironment env = StreamExecutionEnvironment.getExecutionEnvironment();
        DataStreamSource<String> text = env.socketTextStream(host, port, delimiter, maxRetry);

        DataStream<WordWithCount> wordcount = text.flatMap((FlatMapFunction<String, WordWithCount>) (s, collector) -> {
                    String[] words = s.split("\\s");
                    for (String word : words) {
                        collector.collect(new WordWithCount(word, 1L));
                    }
                }).keyBy("word")
                // 执行时间窗口大小为2s，指定时间间隔为1s
                .timeWindow(Time.seconds(2), Time.seconds(1))
                // .sum("count"); // 用sum或者reduce
                .reduce((ReduceFunction<WordWithCount>) (t1, t2) -> {
                    return new WordWithCount(t1.getWord(), t1.getCount() + t2.getCount());
                });

        // 把数据打印到控制台，并且设置并行度
        wordcount.print().setParallelism(1);
        env.execute("滑动窗口单词统计");
    }

    @Data
    @AllArgsConstructor
    private static class WordWithCount {
        private String word;
        private long count;
    }

}
