<script>
  import {SPRITE} from "../lib/markdown.js";
  let {name, small = false} = $props();
</script>

<svg class="icon" class:icon--s={small} aria-hidden="true"><use href="{SPRITE}#i-{name}" /></svg>
