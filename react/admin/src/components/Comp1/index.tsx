// 这是全局引入
// import "./comp1.scss"

// 模块化引入
import styles from "./comp1.module.scss";

const Comp = () => {
  return (
    <div className={styles.box}>
      <p>这是Comp1里面的内容</p>
    </div>
  );
}

export default Comp;
