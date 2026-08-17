import { createRouter, createWebHashHistory } from 'vue-router'
import { isLoggedIn } from '../store/session'

// 后端 brpc 静态服务无 history fallback，必须用 hash 模式
const router = createRouter({
  history: createWebHashHistory(),
  routes: [
    { path: '/', redirect: '/search' },
    { path: '/login', name: 'login', component: () => import('../views/LoginView.vue') },
    {
      path: '/',
      component: () => import('../components/AppLayout.vue'),
      children: [
        {
          path: 'search',
          name: 'search',
          meta: { title: '搜索' },
          component: () => import('../views/SearchView.vue'),
        },
        {
          path: 'collections',
          name: 'collections',
          meta: { title: 'Collections' },
          component: () => import('../views/CollectionsView.vue'),
        },
        {
          path: 'documents',
          name: 'documents',
          meta: { title: '文档管理' },
          component: () => import('../views/DocumentsView.vue'),
        },
        {
          path: 'tenants',
          name: 'tenants',
          meta: { title: '租户管理' },
          component: () => import('../views/TenantsView.vue'),
        },
        {
          path: 'keys',
          name: 'keys',
          meta: { title: '密钥管理' },
          component: () => import('../views/KeysView.vue'),
        },
        {
          path: 'stats',
          name: 'stats',
          meta: { title: '统计' },
          component: () => import('../views/StatsView.vue'),
        },
      ],
    },
    { path: '/:pathMatch(.*)*', redirect: '/search' },
  ],
})

router.beforeEach((to) => {
  if (to.name !== 'login' && !isLoggedIn.value) {
    return { name: 'login', query: { redirect: to.fullPath } }
  }
  if (to.name === 'login' && isLoggedIn.value) {
    return { name: 'search' }
  }
  return true
})

// 会话过期/被吊销：任意请求 401 时统一踢回登录页
window.addEventListener('mss:unauthorized', () => {
  if (router.currentRoute.value.name !== 'login') {
    router.push({ name: 'login' })
  }
})

export default router
