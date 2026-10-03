import {mount} from "svelte";
import "./styles/tokens.css";
import "./styles/base.css";
import App from "./App.svelte";

// The page is as tall as what is visible: on a phone the keyboard takes the bottom away (iOS Safari does not resize
// the layout for it), so the chat's composer stays right above the keyboard.
if (window.visualViewport) {
  const fit = () => {
    document.documentElement.style.setProperty("--page-h", `${visualViewport.height}px`);
    if (scrollY) scrollTo(0, 0);
  };
  visualViewport.addEventListener("resize", fit);
  fit();
}

mount(App, {target: document.getElementById("app")});
