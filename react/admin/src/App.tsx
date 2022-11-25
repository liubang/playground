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
import { useRoutes, Link } from "react-router-dom";
import router from "@/router";

function App() {
  const [count, setCount] = useState(0);
  const outlet = useRoutes(router);

  return (
    <div className="App">
      {outlet}
    </div>
  );
}

export default App;
