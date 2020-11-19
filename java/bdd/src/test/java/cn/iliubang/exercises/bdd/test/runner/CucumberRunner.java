package cn.iliubang.exercises.bdd.test.runner;

import cucumber.api.CucumberOptions;
import cucumber.api.junit.Cucumber;
import org.junit.runner.RunWith;

/**
 * {Insert class description here}
 *
 * @author <a href="mailto:it.liubang@gmail.com">liubang</a>
 * @version $Revision: {Version} $ $Date: 2019-03-16 18:11 $
 */

@RunWith(Cucumber.class)
@CucumberOptions(
        features = "classpath:features",
        tags = {"not @ignored", "@base"},
        plugin = {"json:target/json-report/dw.json", "html:target/cucumber"},
        glue = "classpath:cn.iliubang.exercises.bdd.test.glue"
)
public class CucumberRunner {

}
