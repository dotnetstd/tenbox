<template>
  <footer class="footer">
    <div class="container footer-inner">
      <p class="footer-copy">{{ $t('footer.copyright') }}</p>
    </div>
  </footer>
</template>

<style scoped>
.footer {
  background: var(--color-bg-dark);
  color: rgba(241, 245, 249, 0.6);
  padding: 40px 0;
}

.footer-inner {
  display: flex;
  align-items: center;
  justify-content: center;
}

.footer-copy {
  font-size: 0.875rem;
}
</style>
