//=====================================================================
//
// App.tsx -
//
// Created by liubang on 2022/10/30 23:58
// Last Modified: 2022/10/30 23:58
//
//=====================================================================

import { useState } from "react";
import { Button } from "antd";
import { UpCircleOutlined } from "@ant-design/icons";

function App() {
  const [count, setCount] = useState(0);

  return (
    <div className="App">
      顶级组件
      <br />
      <UpCircleOutlined style={{ fontSize: "30px", color: "Red" }} />
      <Button type="primary">我们的按钮</Button>
    </div>
  );
}

export default App;
