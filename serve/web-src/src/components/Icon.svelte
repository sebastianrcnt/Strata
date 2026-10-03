<script>
  import {SPRITE} from "../lib/markdown.js";
  let {name, size = ""} = $props();
</script>

<svg class="st-icon {size ? `st-icon--${size}` : ''}" aria-hidden="true"><use href="{SPRITE}#i-{name}" /></svg>
