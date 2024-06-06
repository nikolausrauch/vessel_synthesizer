#include "synthesizer.h"

#define GLM_FORCE_RADIANS
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/transform.hpp>
#include <glm/gtx/transform2.hpp>
#include <glm/gtx/rotate_vector.hpp>
#include <glm/ext.hpp>

#include <Eigen/Eigen>

#include <numeric>

namespace vs
{

namespace law
{

/* murrays law for computing radii shrinkage throughout vessel tree */
float murray_radius(float r_l, float r_r, float exponent)
{
    return std::pow(std::pow(r_l, exponent) + std::pow(r_r, exponent), 1.0f/exponent);
}

/* angle computation based on minimal volume in an idialized bifurcation setup */
std::pair<float, float> murray_angles(float r_p, float r_l, float r_r)
{
    float tmp = ( std::pow(r_p, 4.0f) + std::pow(r_l, 4.0f) - std::pow(r_r, 4.0f) ) / ( 2.0f * std::pow(r_p, 2.0f) * std::pow(r_l, 2.0f) );
    tmp = std::min(std::max(tmp, -1.0f), 1.0f);
    float angleOne = - glm::degrees(glm::acos( tmp ));

    tmp = ( std::pow(r_p, 4.0f) - std::pow(r_l, 4.0f) + std::pow(r_r, 4.0f) ) / ( 2.0f * std::pow(r_p, 2.0f) * std::pow(r_r, 2.0f) );
    tmp = std::min(std::max(tmp, -1.0f), 1.0f);
    float angleTwo = glm::degrees(glm::acos( tmp ));

    return { angleOne, angleTwo };
}

std::pair < glm::vec3, glm::vec3 > bets_line_fit(const std::list<synthesizer::attr>& c)
{
    /* copy coordinates to  matrix in Eigen format */
    size_t num_atoms = c.size();
    Eigen::Matrix< double, Eigen::Dynamic, Eigen::Dynamic > centers(num_atoms, 3);

    int i = 0;
    for(auto& p : c)
    {
        centers(i, 0) = p.m_pos.x;
        centers(i, 1) = p.m_pos.y;
        centers(i++, 2) = p.m_pos.z;
    }

    /* find best line by minimizing the orthogonal distances */
    auto mean = centers.colwise().mean();
    glm::vec3 origin(mean(0), mean(1), mean(2));
    Eigen::MatrixXd centered = centers.rowwise() - mean;
    Eigen::MatrixXd cov = centered.adjoint() * centered;
    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> eig(cov);

    auto axis = eig.eigenvectors().col(2).normalized();
    glm::vec3 resaxis(axis(0), axis(1), axis(2));

    return std::make_pair(origin, resaxis);
}

}

synthesizer::system_data::system_data(const glm::vec3 &min, const glm::vec3 &max)
    : m_attr_search(min, max, 32), m_node_search(min, max, 32)
{

}

void synthesizer::system_data::clear()
{
    m_forest.clear();
    m_node_search.clear();

    m_attr_search.clear();
    m_killed_attr.clear();
}

void synthesizer::system_data::clear_attr()
{
    m_attr_search.clear();
    m_killed_attr.clear();
}

synthesizer::synthesizer(domain &tissue)
    : m_domain(tissue),
      m_systems{ system_data(tissue.min_extends(), tissue.max_extends()),
                 system_data(tissue.min_extends(), tissue.max_extends()) }
{

}

void synthesizer::set_settings(const settings &sett)
{
    m_settings = sett;
}

settings& synthesizer::get_settings()
{
    return m_settings;
}

settings::system &synthesizer::get_system_settings(const system sys)
{
    return m_settings.m_system[static_cast<int>(sys)];
}

synthesizer::parameter& synthesizer::get_parameter()
{
    return m_params;
}

synthesizer::parameter::system &synthesizer::get_system_parameter(const system sys)
{
    return m_params.m_system[static_cast<int>(sys)];
}

synthesizer::system_data& synthesizer::get_system_data(const system sys)
{
    return m_systems[static_cast<int>(sys)];
}

void synthesizer::set_forest(const system sys, const forest &other)
{
    auto& sys_data = get_system_data(sys);
    sys_data.clear();

    sys_data.m_forest = other;

    sys_data.m_forest.breadth_first([&](auto& n_tree, auto& n)
    {
        n.data().m_tree = &n_tree; /* TODO: this is so dangerous */
        sys_data.m_node_search.insert(n.data().m_pos, &n);
    });
}


const synthesizer::forest &synthesizer::get_forest(const system sys)
{
    return m_systems[static_cast<int>(sys)].m_forest;
}

synthesizer::tree::node& synthesizer::create_root(const system sys, const glm::vec3 &pos)
{
    auto& sys_data = get_system_data(sys);
    auto& new_tree = sys_data.m_forest.emplace_back();
    auto& root = new_tree.create_root(pos, get_system_settings(sys).m_term_radius, &new_tree);

    sys_data.m_node_search.insert(root.data().m_pos, &root);

    return root;
}

void synthesizer::create_attr(const system sys, const glm::vec3 &pos)
{
    auto& sys_data = get_system_data(sys);
    sys_data.m_attr_search.insert(pos, attr{pos});
}

void synthesizer::try_attr(const system sys, const glm::vec3 &pos)
{
    auto& sys_data = get_system_data(sys);
    const auto& params = get_system_parameter(sys);

    {
        profile_sample(attr_node_query, sys_data.m_profiler);

        std::vector<tree::node*> nodes;
        sys_data.m_node_search.euclidean_range(pos, params.m_birth_node, nodes);
        if(!nodes.empty()) return;
    }

    {
        profile_sample(attr_attr_query, sys_data.m_profiler);

        std::vector<attr> attrs;
        sys_data.m_attr_search.euclidean_range(pos, params.m_birth_attr, attrs);
        if(!attrs.empty()) return;
    }

    sys_data.m_attr_search.insert(pos, attr{pos});
}

void synthesizer::run()
{
    /* profiling is enabled */
    if constexpr (prf::monitor::is_enabled)
    {
        get_system_data(system::arterial).m_profiler.reset();
        get_system_data(system::venous).m_profiler.reset();
    }

    if(get_system_data(system::arterial).m_forest.trees().empty())
    {
        return;
    }

    init_runtime_params();

    m_is_running.store(true);

    /* main simulation loop */
    while( (m_params.m_curr_step++ < m_settings.m_steps) && m_is_running.load())
    {
        /* profiling is enabled */
        if constexpr (prf::monitor::is_enabled)
        {
            get_system_data(system::arterial).m_profiler.start_frame();
            get_system_data(system::venous).m_profiler.start_frame();
        }

        {
            profile_sample(total_arterial, get_system_data(system::arterial).m_profiler);
            profile_sample(total_venous, get_system_data(system::venous).m_profiler);

            /* place new oxygen-drains for arterial system to reach */
            sample_attraction();
            /* develop arterial system */
            step(system::arterial);
            /* if oxygen-drains are satisfied set as carbon-dioxide sources */
            combine_systems();
            /* develop venous system to reach carbon-dioxide sources */
            step(system::venous);

            /* scale domain by modifying distance parameters */
            domain_growth(system::arterial);
            domain_growth(system::venous);
        }

        /* profiling is enabled */
        if constexpr (prf::monitor::is_enabled)
        {
            get_system_data(system::arterial).m_profiler.end_frame();
            get_system_data(system::venous).m_profiler.end_frame();
        }
    }

    m_is_running.store(false);
}

void synthesizer::init_runtime_params()
{
    m_params.m_curr_step = 0;

    for(unsigned int i = 0; i < static_cast<int>(system::count); i++)
    {
        m_params.m_system[i].m_scaling = 1.0f;

        m_params.m_system[i].m_birth_attr = m_settings.m_system[i].m_birth_attr;
        m_params.m_system[i].m_birth_node = m_settings.m_system[i].m_birth_node;

        m_params.m_system[i].m_influence_attr = m_settings.m_system[i].m_influence_attr;
        m_params.m_system[i].m_kill_attr = m_settings.m_system[i].m_kill_attr;

        m_params.m_system[i].m_growth_distance = m_settings.m_system[i].m_growth_distance;
    }
}

void synthesizer::step(const system sys)
{
    auto& data = get_system_data(sys);
    if(data.m_forest.trees().empty()) { return; }

    profile_sample(step, data.m_profiler);

    std::map<tree::node*, std::list<attr>> attr_map;

    /* get closest nodes to attraction points while satisfying the different criteria */
    step_closest(sys, attr_map);

    /* grow vessels based on associated attraction points */
    step_growth(sys, attr_map);

    /* remove attraction points which are too close */
    step_kill(sys, attr_map);
}

void synthesizer::sample_attraction()
{
    profile_sample(step_closest, get_system_data(system::arterial).m_profiler);

    std::vector<glm::vec3> points;
    m_domain.get().samples(points, get_settings().m_sample_count);
    std::for_each(points.begin(), points.end(), [&](const auto& p) { try_attr(system::arterial, p); });
}

void synthesizer::step_closest(const system sys, std::map<tree::node*, std::list<attr> >& attr_map)
{
    auto& data = get_system_data(sys);
    auto& params = get_system_parameter(sys);
    auto& sett = get_system_settings(sys);

    profile_sample(step_closest, data.m_profiler);

    /* check all attraction points if they are influence a vessel node */
    std::vector<tree::node*> nodes;
    data.m_attr_search.traverse([&](const attr& p)
    {
        {
            profile_sample(influence_query, data.m_profiler);

            nodes.clear();
            data.m_node_search.euclidean_range(p.m_pos, params.m_influence_attr, nodes);
            if(nodes.empty()) { return; }
        }

        float min = std::numeric_limits<float>::max();
        tree::node* min_node = nullptr;

        std::for_each(nodes.begin(), nodes.end(), [&](auto* curr_node)
        {
            assert(curr_node != nullptr);

            if(curr_node->is_joint()) return;

            float distance = glm::length(p.m_pos - curr_node->data().m_pos);
            if(distance < min)
            {
                min = distance;
                min_node = curr_node;
            }
        });

        profile_sample(influence_filter, data.m_profiler);

        /* if attr point is in influence range */
        if(min_node)
        {
            /* Filter perception volume leaf */
            if(!min_node->is_root() && !min_node->is_inter())
            {
                auto& parent = min_node->data().m_tree->get_node(min_node->parent());
                glm::vec3 d_parent = glm::normalize(min_node->data().m_pos - parent.data().m_pos);
                glm::vec3 d_attr = glm::normalize( p.m_pos - min_node->data().m_pos );

                float dot_product = glm::dot(d_parent, d_attr);
                float angle = glm::degrees(glm::acos(dot_product));

                if(angle > sett.m_percept_vol*0.5f ) { return; }
            }

            /* Filter perception volume internode */
            if(!min_node->is_root() && min_node->is_inter())
            {
                auto& parent = min_node->data().m_tree->get_node(min_node->parent());
                glm::vec3 d_parent = glm::normalize(min_node->data().m_pos - parent.data().m_pos);
                glm::vec3 d_attr = glm::normalize( p.m_pos - min_node->data().m_pos );

                float dot_product = glm::dot(d_parent, d_attr);
                float angle = glm::degrees(glm::acos(dot_product));

                auto& child_0 = min_node->data().m_tree->get_node(min_node->children()[0]);
                float parent_radius = law::murray_radius(child_0.data().m_radius, sett.m_term_radius, sett.m_bif_index);
                float perfect_angle = std::fabs(law::murray_angles(parent_radius, child_0.data().m_radius, sett.m_term_radius).second);

                if( std::fabs(angle - perfect_angle) > (sett.m_percept_vol*0.5f) )
                    return;
            }

            /* if it passes all tests it gets added to points influencing this node */
            attr_map[min_node].push_back(p);
        }
    });
}

void synthesizer::step_growth(const system sys, std::map<tree::node *, std::list<attr> >& attr_map)
{
    auto& data = get_system_data(sys);
    auto& params = get_system_parameter(sys);
    auto& sett = get_system_settings(sys);

    profile_sample(step_growth, data.m_profiler);

    for(const auto& attr_pair : attr_map)
    {
        auto* node = attr_pair.first;
        const std::list<attr>& attr_list = attr_pair.second;

        /* get average direction vector of attr point directions */
        glm::vec3 dir = std::accumulate(attr_list.begin(), attr_list.end(), glm::vec3{0.0, 0.0, 0.0}, [&node] (const auto dir, auto& att)
        {
            return dir + glm::normalize(att.m_pos - node->data().m_pos);
        });
        dir = glm::normalize(dir);


        /* collect bias direction and detect if bifurcation development is preferred */
        bool bifurcation = false;
        if(!node->is_root())
        {
            profile_sample(growth_bias_dir, data.m_profiler);

            auto& parent = node->data().m_tree->get_node(node->parent());
            glm::vec3 d_parent = glm::normalize(node->data().m_pos - parent.data().m_pos);

            if(node->is_leaf() && attr_list.size() > 1 && sett.m_bif_thresh >= 0.0f)
            {
                Eigen::MatrixXf angles(1, attr_list.size());

                int i = 0;
                for(auto& att : attr_list)
                {
                    glm::vec3 dir_vec = glm::normalize(att.m_pos - node->data().m_pos);
                    float angle = glm::degrees(glm::acos( glm::dot(d_parent, dir_vec) ) );

                    angles(0, i++) = angle;
                }

                float mean = angles.row(0).mean();
                angles.row(0).array() -= mean;
                float sd = std::sqrt( (angles.row(0).array().cwiseProduct(angles.row(0).array())).sum() );

                bifurcation = (sd >= sett.m_bif_thresh);
            }

            glm::vec3 bias = dir;
            if(node->is_leaf())
            {
                bias = d_parent;
            }
            else if(node->is_inter())
            {
                auto& child_0 = node->data().m_tree->get_node(node->children()[0]);
                float parent_radius = law::murray_radius(child_0.data().m_radius, sett.m_term_radius, sett.m_bif_index);
                float perfect_angle = std::fabs(law::murray_angles(parent_radius, child_0.data().m_radius, sett.m_term_radius).second);

                glm::vec3 normal = glm::normalize( glm::cross(d_parent, dir) );

                bias = glm::normalize(glm::rotate(d_parent, glm::radians(perfect_angle), normal));
            }

            dir = glm::normalize( (1.0f - sett.m_parent_inertia) * dir + sett.m_parent_inertia * bias );
        }

        /* develop a bifurcation from a leaf (--> threshold was met) */
        if( node->is_leaf() && bifurcation )
        {
            profile_sample(growth_bifurcations, data.m_profiler);

            auto& parent = node->data().m_tree->get_node(node->parent());
            glm::vec3 d_parent = glm::normalize(node->data().m_pos - parent.data().m_pos);

            float radius_l = sett.m_term_radius;
            float radius_r = sett.m_term_radius;
            float parent_radius = law::murray_radius(radius_l, radius_r, sett.m_bif_index);

            auto [angle_l, angle_r] = law::murray_angles(parent_radius, radius_l, radius_r);

            auto line = law::bets_line_fit(attr_list);
            glm::vec3 up = glm::cross(glm::normalize(line.first - node->data().m_pos), line.second);
            glm::vec3 dir = d_parent;

            glm::vec3 left = glm::normalize(glm::rotate(dir, glm::radians(angle_l), up));
            glm::vec3 right = glm::normalize(glm::rotate(dir, glm::radians(angle_r), up));

            auto* tree = node->data().m_tree;
            auto& end_l = tree->create_node(node->id(), node->data().m_pos + params.m_growth_distance * glm::normalize(left), radius_l, tree);
            auto& end_r = tree->create_node(node->id(), node->data().m_pos + params.m_growth_distance * glm::normalize(right), radius_r, tree);

            auto recalc_radii = [&sett, tree] (auto& node)
            {
                if(node.is_inter())
                {
                    node.data().m_radius = tree->get_node(node.children()[0]).data().m_radius;
                }
                else if(node.is_joint())
                {
                    auto& child_0 = node.data().m_tree->get_node(node.children()[0]);
                    auto& child_1 = node.data().m_tree->get_node(node.children()[1]);

                    node.data().m_radius = law::murray_radius(child_0.data().m_radius, child_1.data().m_radius, sett.m_bif_index);
                }
            };
            tree->to_root(recalc_radii, node->id());

            data.m_node_search.insert(end_l.data().m_pos, &end_l);
            data.m_node_search.insert(end_r.data().m_pos, &end_r);
        }
        /* elongate from a leaf or develop a new lateral sprout */
        else if( !sett.m_only_leaf_development || (node->is_leaf() || node->is_inter()) )
        {
            if(node->is_root() && node->is_inter()) { continue; } // TODO: currently force root to only have one child

            profile_sample(growth_sprout, data.m_profiler);

            auto* tree = node->data().m_tree;
            auto& end = tree->create_node(node->id(), node->data().m_pos + params.m_growth_distance * glm::normalize(dir), sett.m_term_radius, tree);

            auto recalc_radii = [&sett, tree] (auto& node)
            {
                if(node.is_inter())
                {
                    node.data().m_radius = tree->get_node(node.children()[0]).data().m_radius;
                }
                else if(node.is_joint())
                {
                    auto& child_0 = node.data().m_tree->get_node(node.children()[0]);
                    auto& child_1 = node.data().m_tree->get_node(node.children()[1]);

                    node.data().m_radius = law::murray_radius(child_0.data().m_radius, child_1.data().m_radius, sett.m_bif_index);
                }
            };
            tree->to_root(recalc_radii, node->id());

            data.m_node_search.insert(end.data().m_pos, &end);
        }
    }
}

void synthesizer::step_kill(const system sys, std::map<tree::node*, std::list<attr> > &attr_map)
{
    auto& data = get_system_data(sys);
    auto& params = get_system_parameter(sys);

    profile_sample(step_kill, data.m_profiler);

    for(const auto& attrPair : attr_map)
    {
        const std::list<attr>& attr_list = attrPair.second;

        /* check if attr points that have influenced nodes are considered as satisfied */
        std::vector<tree::node*> nodes;
        for(const auto& p : attr_list)
        {
            {
                profile_sample(kill_node_query, data.m_profiler);
                data.m_node_search.euclidean_range(p.m_pos, params.m_kill_attr, nodes);
            }

            if(nodes.empty()) continue;

            {
                profile_sample(kill_attr_remove, data.m_profiler);
                data.m_attr_search.remove(p.m_pos, p);
                data.m_killed_attr.push_back(p.m_pos);
            }
        }
    }
}

void synthesizer::combine_systems()
{
    auto& art_data = get_system_data(system::arterial);
    auto& ven_data = get_system_data(system::venous);

    if(ven_data.m_forest.trees().empty()) { return; }

    profile_sample(create_carbon_dioxide, art_data.m_profiler);

    /* if combined growth of systems; place satisfied oxygen-drains as carbon-dioxide sources */
    for(const auto& pos : art_data.m_killed_attr)
    {
        create_attr(system::venous, pos);
    }
    art_data.m_killed_attr.clear();
}

void synthesizer::domain_growth(const system sys)
{
    profile_sample(domain_growth, get_system_data(sys).m_profiler);

    auto& sett = get_system_settings(sys);
    auto& params = get_system_parameter(sys);
    float* scaling = &params.m_scaling;

    switch (sett.m_grow_func.m_type) {
    case grow_func::none :
        break;
    case grow_func::linear :
        *scaling += sett.m_grow_func.m_value;
        break;
    case grow_func::exponential :
        *scaling += *scaling * sett.m_grow_func.m_value;
        break;
    }

    /* scale domain distance values */
    float inverseScale = 1.0f / *scaling;

    params.m_birth_attr = sett.m_birth_attr * inverseScale;
    params.m_birth_node = sett.m_birth_node * inverseScale;
    params.m_influence_attr = sett.m_influence_attr * inverseScale;
    params.m_kill_attr = sett.m_kill_attr * inverseScale;
    params.m_growth_distance = sett.m_growth_distance * inverseScale;
}

void settings::scale(float s)
{
    for(unsigned int i = 0; i < static_cast<int>(vs::system::count); i++)
    {
        m_system[i].m_birth_attr *= s;
        m_system[i].m_birth_node *= s;

        m_system[i].m_term_radius *= s;
        m_system[i].m_growth_distance *= s;

        m_system[i].m_influence_attr *= s;
        m_system[i].m_kill_attr *= s;
    }
}

settings::system& settings::get_system_data(const vs::system sys)
{
    return m_system[static_cast<int>(sys)];
}


}
