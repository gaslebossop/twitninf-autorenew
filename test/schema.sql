-- Schéma d'essai : les tables de l'API TwitNinf que le démon touche, réduites
-- aux colonnes qu'il lit ou écrit, mais avec les MÊMES types.
--
-- Les ENUM sont recréés sous leurs noms Sequelize réels : le démon passe
-- 'plus'/'TRANSFER'/'premium' comme paramètres texte et compte sur la
-- déduction de type par la colonne cible. Un essai sur des colonnes TEXT ne
-- vérifierait pas ce point, qui est exactement le genre de détail qui casse en
-- production et nulle part ailleurs.

DROP TABLE IF EXISTS subscription_mandates, notifications, transactions,
                     transaction_risk_authorizations, user_wallets,
                     virtual_currencies, users CASCADE;
DROP TYPE IF EXISTS enum_users_subscription_tier, enum_transactions_type,
                    enum_transactions_status, enum_notifications_type,
                    enum_notifications_priority CASCADE;

CREATE TYPE enum_users_subscription_tier AS ENUM ('free', 'plus', 'pro', 'ultra');
CREATE TYPE enum_transactions_type       AS ENUM ('TRANSFER', 'MINING', 'PURCHASE', 'REWARD', 'REFUND', 'SYSTEM');
CREATE TYPE enum_transactions_status     AS ENUM ('PENDING', 'COMPLETED', 'FAILED', 'CANCELLED');
CREATE TYPE enum_notifications_type      AS ENUM ('like','retweet','reply','mention','follow','unfollow','quote','system','verification','premium');
CREATE TYPE enum_notifications_priority  AS ENUM ('low', 'normal', 'high', 'urgent');

CREATE TABLE users (
  id                       UUID PRIMARY KEY,
  username                 VARCHAR(50) NOT NULL,
  premium                  BOOLEAN NOT NULL DEFAULT false,
  subscription_tier        enum_users_subscription_tier NOT NULL DEFAULT 'free',
  subscription_expires_at  TIMESTAMPTZ NULL,
  tweet_generation_credits INTEGER NULL DEFAULT 0,
  g_auth_sub               TEXT NULL,
  -- Sanctions : le démon les écrit lorsqu'un mandat tombe en défaut de
  -- paiement. `suspension_meta` est bien du JSONB dans le modèle Sequelize —
  -- une colonne TEXT laisserait passer un jsonb_build_object mal formé.
  ban_count                INTEGER NOT NULL DEFAULT 0,
  is_suspended             BOOLEAN NOT NULL DEFAULT false,
  suspended_at             TIMESTAMPTZ NULL,
  suspended_until          TIMESTAMPTZ NULL,
  suspension_reason        TEXT NULL,
  suspension_meta          JSONB NOT NULL DEFAULT '{}'::jsonb,
  created_at               TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  updated_at               TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE TABLE virtual_currencies (
  id            UUID PRIMARY KEY,
  symbol        VARCHAR(10) NOT NULL,
  current_price NUMERIC(10,4) NOT NULL DEFAULT 1.0,
  is_active     BOOLEAN NOT NULL DEFAULT true,
  created_at    TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE TABLE user_wallets (
  id              UUID PRIMARY KEY,
  user_id         UUID NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  currency_id     UUID NOT NULL REFERENCES virtual_currencies(id),
  balance         NUMERIC(20,8) NOT NULL DEFAULT 0,
  total_earned    NUMERIC(20,8) NOT NULL DEFAULT 0,
  total_spent     NUMERIC(20,8) NOT NULL DEFAULT 0,
  total_purchased NUMERIC(20,8) NOT NULL DEFAULT 0,
  loyalty_points  INTEGER NOT NULL DEFAULT 0,
  is_locked       BOOLEAN NOT NULL DEFAULT false,
  lock_reason     VARCHAR(255) NULL,
  created_at      TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  updated_at      TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  UNIQUE (user_id, currency_id)
);

CREATE TABLE transactions (
  id               UUID PRIMARY KEY,
  transaction_hash VARCHAR(64) NOT NULL UNIQUE,
  from_user_id     UUID NULL REFERENCES users(id),
  to_user_id       UUID NOT NULL REFERENCES users(id),
  currency_id      UUID NOT NULL REFERENCES virtual_currencies(id),
  amount           NUMERIC(20,8) NOT NULL,
  amount_in_eur    NUMERIC(20,8) NOT NULL,
  type             enum_transactions_type NOT NULL,
  status           enum_transactions_status NOT NULL DEFAULT 'PENDING',
  fee              NUMERIC(20,8) NOT NULL DEFAULT 0,
  description      TEXT NULL,
  metadata         JSONB NOT NULL DEFAULT '{}'::jsonb,
  confirmed_at     TIMESTAMPTZ NULL,
  created_at       TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  updated_at       TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE TABLE transaction_risk_authorizations (
  id         UUID PRIMARY KEY,
  user_id    UUID NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  decision   VARCHAR(20) NULL,
  status     VARCHAR(20) NOT NULL DEFAULT 'PENDING',
  created_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE TABLE notifications (
  id           UUID PRIMARY KEY,
  recipient_id UUID NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  sender_id    UUID NULL REFERENCES users(id),
  type         enum_notifications_type NOT NULL,
  title        VARCHAR(100) NOT NULL,
  message      TEXT NOT NULL,
  content      JSONB NULL DEFAULT '{}'::jsonb,
  is_read      BOOLEAN NOT NULL DEFAULT false,
  read_at      TIMESTAMPTZ NULL,
  priority     enum_notifications_priority NOT NULL DEFAULT 'normal',
  metadata     JSONB NOT NULL DEFAULT '{}'::jsonb,
  created_at   TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  updated_at   TIMESTAMPTZ NOT NULL DEFAULT NOW()
);
