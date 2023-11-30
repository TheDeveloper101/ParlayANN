// Yinyang method for accelerating exact kmeans

#ifndef YY
#define YY

#include "distance.h"         //for distance object
#include "initialization.h"   //for Lazy initialization
#include "kmeans_bench.h"     //for logger object
#include "naive.h"            //for NaiveKmeans method (we use internally)
#include "parlay/internal/get_time.h"   //for the timer
#include "parlay/primitives.h"   //for assorted parlay functions (histogram, tabulate, etc.)
#include "lsh.h"

// T: point data type, Point: Point object type, index_type: type of assignment
// value (for asg, e.g., size_t), CT: type of center data, CenterPoint: Center
// Point object
template <typename T, typename Point, typename index_type, typename CT,
          typename CenterPoint>
struct Yinyang : KmeansInterface<T, Point, index_type, CT, CenterPoint> {

  //param of Yinyang: choose whether to group points
  bool do_point_groups=false;
  bool do_center_groups = false;

  struct point {
    index_type best;   // the index of the best center for the point
    parlay::slice<T*, T*> coordinates;   // the coordinates of the point
    index_type id;   // an id used for the point, for debugging purposes
    float ub;
    float global_lb;       // global lower bound
    index_type old_best;   // the previous best
    index_type group_id;

    point(index_type id, index_type chosen, parlay::slice<T*, T*> coordinates)
        : best(chosen),
          coordinates(coordinates.begin(), coordinates.end()),
          id(id),
          ub(std::numeric_limits<float>::max()),
          global_lb(-1),
          old_best(chosen) {}
  };

  struct center {
    index_type id;         // a unique identifier for the center
    index_type group_id;   // the id of the group that the center belongs to
    parlay::sequence<CT> coordinates;   // the coordinates of the center
    float delta;
    index_type old_num_members;   // how many points belonged to that center
    index_type new_num_members;   // the number of members this iter
    bool has_changed;             // check if the center has changed

    center(index_type id, CT* coords, size_t d)
        : id(id),
          group_id(-1),
          coordinates(parlay::tabulate(d, [&](size_t i) { return coords[i]; })),
          delta(0),
          old_num_members(0),
          new_num_members(0),
          has_changed(true) {}

    CT* begin() { return coordinates.begin(); }
  };

  struct group {
    index_type id;
    parlay::sequence<index_type>
       member_ids;   // store the ids of all the centers (or points, in point grouping) belonging to this group
    float max_drift;
    float global_lb;

    group(index_type id)
        : id(id), member_ids(parlay::sequence<index_type>()), max_drift(-1) {}
  };

  // initialize the groups by running a naive kmeans a few times
  void init_groups(size_t d, size_t ad, size_t k, CT* c,
                   parlay::sequence<center>& centers,
                   parlay::sequence<group>& groups, size_t t, Distance& D) {
    CT* group_centers = new CT[t * ad];
    index_type* group_asg = new index_type[k];
    size_t max_iter = 5;
    double epsilon = 0;

    Lazy<CT, CT, index_type> init;
    init(c, k, d, ad, t, group_centers, group_asg);

    kmeans_bench logger = kmeans_bench(k, d, t, max_iter, epsilon,
                                       "Lazy currently", "YY's Internal Naive");
    logger.start_time();

    NaiveKmeans<CT, Euclidian_Point<CT>, index_type, CT, Euclidian_Point<CT>>
       run;
    run.cluster_middle(c, k, d, ad, t, group_centers, group_asg, D, logger,
                       max_iter, epsilon, true);

    logger.end_time();

    // occasionally, for example when k is high and d=1 on int data, the kmeans
    // clustering will leave a group empty. In this case, we opt for random
    // group assignments instead. TODO handle this case more elegantly (for
    // example transferring ownership of centers from a large group into an
    // empty group)
    parlay::sequence<size_t> group_repped(t, 0);
    for (size_t i = 0; i < k; i++) {
      group_repped[group_asg[i]] = 1;
    }
    if (parlay::reduce(group_repped) < t) {
      std::cout << "Standard group init with kmeans clustering failed. Opting "
                   "for random init."
                << std::endl;
      parlay::parallel_for(0, k,
                           [&](size_t i) { centers[i].group_id = i % t; });
      for (size_t i = 0; i < k; i++) {
        groups[centers[i].group_id].member_ids.push_back(i);
      }
    } else {
      parlay::parallel_for(
         0, k, [&](size_t i) { centers[i].group_id = group_asg[i]; });
      for (size_t i = 0; i < k; i++) {   // sequential assigning the groups
                                         // their centers, TODO do in parallel?
        groups[centers[i].group_id].member_ids.push_back(i);
      }
    }

    delete[] group_centers;
    delete[] group_asg;
  }

  void assert_proper_point_group_size(size_t k,
                                const parlay::sequence<point>& centers,
                                const parlay::sequence<group>& groups, size_t t,
                                bool DEBUG_FLAG = false) {
    for (size_t i = 0; i < t; i++) {
      if (groups[i].member_ids.size() == 0) {
        std::cout << "Group assignment went wrong, group is empty" << std::endl;
        std::cout << i << " " << groups[i].member_ids.size() << std::endl;
        abort();
      }
    }

    size_t total_centers_accounted = 0;
    for (size_t j = 0; j < t; j++) {
      for (size_t elt = 0; elt < groups[j].member_ids.size(); elt++) {
        if (groups[j].member_ids[elt] >= k || groups[j].member_ids[elt] < 0) {
          std::cout << "invalid center in group asg, error ";
          abort();
        }
      }
      total_centers_accounted += groups[j].member_ids.size();
    }
    if (total_centers_accounted != k) {
      std::cout << "only " << total_centers_accounted << "in groups."
                << std::endl;
      abort();
    }
    for (size_t i = 0; i < k; i++) {
      if (centers[i].group_id < 0 || centers[i].group_id >= t) {
        std::cout << "center " << i << "has invalid group id "
                  << centers[i].group_id << ", aborting." << std::endl;
        abort();
      }
    }

                                }

  // confirm that the groups are nonempty
  void assert_proper_group_size(size_t k,
                                const parlay::sequence<center>& centers,
                                const parlay::sequence<group>& groups, size_t t,
                                bool DEBUG_FLAG = false) {

    for (size_t i = 0; i < t; i++) {
      if (groups[i].member_ids.size() == 0) {
        std::cout << "Group assignment went wrong, group is empty" << std::endl;
        std::cout << i << " " << groups[i].member_ids.size() << std::endl;
        abort();
      }
    }

    size_t total_centers_accounted = 0;
    for (size_t j = 0; j < t; j++) {
      for (size_t elt = 0; elt < groups[j].member_ids.size(); elt++) {
        if (groups[j].member_ids[elt] >= k || groups[j].member_ids[elt] < 0) {
          std::cout << "invalid center in group asg, error ";
          abort();
        }
      }
      total_centers_accounted += groups[j].member_ids.size();
    }
    if (total_centers_accounted != k) {
      std::cout << "only " << total_centers_accounted << "in groups."
                << std::endl;
      abort();
    }
    for (size_t i = 0; i < k; i++) {
      if (centers[i].group_id < 0 || centers[i].group_id >= t) {
        std::cout << "center " << i << "has invalid group id "
                  << centers[i].group_id << ", aborting." << std::endl;
        abort();
      }
    }
  }
  // make sure that the centers collectively own exactly n points (assertion)
  void assert_members_n(size_t n, size_t k,
                        const parlay::sequence<center>& centers) {
    size_t elt_counter =
       parlay::reduce(parlay::map(centers, [&](const center& my_center) {
         return my_center.old_num_members;
       }));

    if (elt_counter != n) {
      std::cout << "error in num_members assignment: " << elt_counter
                << std::endl;
      abort();
    }
  }
  // ptr dist is distance between two pointers
  float ptr_dist(CT* a, CT* b, size_t d, Distance& D) {
    return std::sqrt(D.distance(a, b, d));
  }

  // find distance between point (p) and center
  float pc_dist_squared(point& p, size_t d, center& cen, Distance& D) {
    CT buf[2048];
    T* it = p.coordinates.begin();
    for (size_t j = 0; j < d; j++)
      buf[j] = *(it++);
    return D.distance(buf, cen.begin(), d);
  }

  void init_set_closest_point(point& p, size_t d, size_t k, parlay::sequence<center>& centers, Distance& D, parlay::sequence<float>& lbs) {
   
      CT buf[2048];
      T* it = p.coordinates.begin();
      for (size_t j = 0; j < d; j++)
        buf[j] = *(it++);
      // first, we find the closest center to each point
      auto distances = parlay::delayed::map(
        centers, [&](center& q) { return D.distance(buf, q.begin(), d); });

      p.best = min_element(distances) - distances.begin();
      p.old_best = p.best;
      p.ub = std::sqrt(distances[p.best]);

      // TODO sadly linear in k, improve?
      for (size_t j = 0; j < k; j++) {
        float new_lb = std::sqrt(distances[j]);
        // TODO using an if check instead of a min, faster?
        if (j != p.best && new_lb < lbs[centers[j].group_id])
          lbs[centers[j].group_id] = new_lb;
      }

  }
  // Init the point bounds
  void init_point_bounds(parlay::sequence<point>& pts, size_t n, size_t d, size_t ad, size_t k, parlay::sequence<center>& centers, Distance& D, parlay::sequence<parlay::sequence<float>>& lbs,parlay::sequence<group> point_groups,size_t npg,bool do_point_groups) {
    if (do_point_groups) {
      parlay::parallel_for(0,npg,[&] (size_t pg) {
        for (size_t i : point_groups[pg].member_ids) {
          init_set_closest_point(pts[i],d,k,centers,D,lbs[pts[i].group_id]);

        }
      });

    }
    else {
      parlay::parallel_for(0, n, [&](size_t i) {
        init_set_closest_point(pts[i],d,k,centers,D,lbs[i]);
        
      });
    }
  }

  void assign_step(parlay::sequence<point>& pts, size_t n, size_t d, size_t ad, size_t k, parlay::sequence<center>& centers, parlay::sequence<group>& groups, size_t t, Distance& D, parlay::sequence<parlay::sequence<float>>& lbs, parlay::sequence<size_t>& distance_calculations, parlay::sequence<int>& center_reassignments, parlay::sequence<group>& point_groups, size_t npg) {
    //if we are using point groups
    if (do_point_groups) {
      //for each point group
      parlay::parallel_for(0,npg,[&] (size_t pg) {

        //first, subtract the group drift from each of the lbs
        for (size_t j = 0; j < t; j++) lbs[pg][j] = std::max(static_cast<float>(0), lbs[pg][j] - groups[j].max_drift);
        
        //calculate the highest point ub, we call farthest_dist_from_center
        float farthest_dist_from_center = *parlay::max_element(parlay::map(point_groups[pg].member_ids,[&] (size_t i) {
          return pts[i].ub;
        }));

        float lowest_lb = *parlay::min_element(lbs[pg]); //calculate the lowest lb
        //if all are points are closer to their centers than the global lb we can stop here
        if (farthest_dist_from_center <= lowest_lb) return;

        //otherwise, tighten the point bounds
        //note this hides a buffer copy TODO improve
        for (size_t i : point_groups[pg].member_ids) pts[i].ub=std::sqrt(pc_dist_squared(pts[i],d,centers[pts[i].best],D));
    
        //then check again if the highest ub is <= the lowest lb
        farthest_dist_from_center = *parlay::max_element(parlay::map(point_groups[pg].member_ids,[&] (size_t i) {
          return pts[i].ub;
        }));
        lowest_lb = *parlay::min_element(lbs[pg]);

        if (farthest_dist_from_center <= lowest_lb) return;
        //group filtering stage on the point block
        for (size_t j = 0; j < t; j++) {
          //if all the points close enough, skip
          if (farthest_dist_from_center <= lbs[pg][j]) continue;

          //otherwise, calculate all the distances from each point to each center
          lbs[pg][j] = std::numeric_limits<float>::max(); //reset the lb for this point group - group combo
          for (size_t i : point_groups[pg].member_ids) { //for each point in the p-group
            point& p = pts[i];
            CT buf[2048];
            T* it = p.coordinates.begin();
            for (size_t j = 0; j < d; j++) buf[j] = *(it++);
            for (size_t l = 0; l < groups[j].member_ids.size(); l++) {
              size_t c_id = groups[j].member_ids[l];
              if (p.best==c_id) continue;

              float new_d = ptr_dist(buf, centers[c_id].begin(), d,
              D);   
              distance_calculations[p.id]++;
              if (p.ub > new_d) {
                p.best=c_id;
                p.ub=new_d;
                //lb of old center may tighten here 
                if (lbs[pg][centers[p.best].group_id] > p.ub) {
                  lbs[pg][centers[p.best].group_id] = p.ub;
                }

              }
              //lower the lb if we can
              else if (lbs[pg][j] > new_d) {
                lbs[pg][j]=new_d;

              }


            }

          }          

        }

      });

    }
    else {
      parlay::parallel_for(0,n,[&] (size_t i) {

        //subtract the group drift
        for (size_t j = 0; j < t; j++) {
        lbs[i][j] =
            std::max(static_cast<float>(0), lbs[i][j] - groups[j].max_drift);
      
        }

        point& p = pts[i];

        p.ub += centers[p.best].delta;
        p.old_best = p.best;

        p.global_lb = *parlay::min_element(lbs[i]);

        // nothing happens if our closest center can't change
        if (p.global_lb >= p.ub)
          return;

        // even though we have a dist wrapper, we copy the point to a buffer
        // here, so that we only have to copy the point to a buffer once,
        // instead of the k times needed if we called dist k times
        CT buf[2048];
        T* it = p.coordinates.begin();
        for (size_t j = 0; j < d; j++)
          buf[j] = *(it++);

        p.ub = ptr_dist(buf, centers[p.best].begin(), d, D);
        distance_calculations[p.id] += 1;

        // again, nothing happens if our closest center can't change
        if (p.global_lb >= p.ub)
          return;

        // for each group
        for (size_t j = 0; j < t; j++) {
          // if group j is too far away we don't look at it
          if (p.ub <= lbs[i][j])
            continue;

          // reset the lower bound, make it smallest distance we calculate
          // that's not the closest distance away note that this max set can
          // overwrite the lbs[i][j] being set to the old ub on a center
          // reassignment, so we do need to do a distance calculation with the
          // old_best to recover that bound
          //lbs[i][j] = std::numeric_limits<float>::max();
          //TODO fix me put lbs back in place!
          lbs[i][j] = std::numeric_limits<float>::max();

          // for each group member (center)
          for (size_t l = 0; l < groups[j].member_ids.size(); l++) {
            // c_id is the id of our candidate center, creating variable for
            // ease of reading
            size_t c_id = groups[j].member_ids[l];

            // don't do a distance comparison with the current best
            if (p.best == c_id)
              continue;

            float new_d = ptr_dist(buf, centers[c_id].begin(), d,
                                   D);   // find distance to center l in group j
            distance_calculations[p.id]++;   // increment distance calc counter for
                                          // this pt

            // note that the ub is tight rn, that ub IS the distance to the
            // previously closest center So if our new dist is less than ub, we
            // have a new closest point
            if (p.ub > new_d) {

              // update the lower bound of the group with the previous best
              // center
              if (lbs[i][centers[p.best].group_id] > p.ub)
                lbs[i][centers[p.best].group_id] = p.ub;

              p.best = c_id;
              p.ub = new_d;   // new ub is tight

            
            } else {
              // if this center is not the closest, use it to improve the lower
              // bound
              if (lbs[i][j] > new_d) {
                lbs[i][j] = new_d;
              }
            }
          }
        }
        
      });
    }
     
  }

  // run yy
  void cluster_middle(T* v, size_t n, size_t d, size_t ad, size_t k, CT* c,
                      index_type* asg, Distance& D, kmeans_bench& logger,
                      size_t max_iter, double epsilon,
                      bool suppress_logging = false) {

    parlay::internal::timer tim = parlay::internal::timer();
    tim.start();
    float assignment_time = 0;
    float update_time = 0;
    float setup_time = 0;

    if (max_iter == 0)
      return;   // when we do no iterations, nothing needs to happen at all

    if (d > 2048) {   // we copy a point into a float buffer of fixed size 2048,
                      // so we don't support d > 2048
      std::cout << "d greater than 2048, too big, printing d: " << d
                << std::endl;
      abort();
    }

    parlay::sequence<center> centers =
       parlay::tabulate<center>(k, [&](size_t i) {   // create the centers
         return center(i, c + i * ad, d);
       });

    // t is the number of groups (notation following paper)
    // We want t to be big without overloading memory because our memory cost
    // contains O(nt). Leaving at t=k/10 for now to allow for more consistent
    // benching, but should fiddle with, TODO optimize
    size_t t;
    //flag for whether or not we group centers. t=k is equivalent of each center being its own group (so effectively Elkan's-like, no grouping)
    if (do_center_groups) { 
      t = std::max(static_cast<size_t>(1), k / 10);
    }
    else {
      t=k;
    }
    

    parlay::sequence<group> groups =
       parlay::tabulate<group>(t, [&](size_t i) { return group(i); });

    init_groups(d, ad, k, c, centers, groups, t, D);

   


    assert_proper_group_size(k, centers, groups, t,
                             false);   // confirm groups all nonempty

    parlay::sequence<point> pts = parlay::tabulate<point>(n, [&](size_t i) {
      return point(i, asg[i], parlay::slice(v + i * ad, v + i * ad + d));
    });

     //npg = number of point groups. npg is to points as t is to centers.
    size_t npg = n/10;//std::max(static_cast<size_t>(1),n/50);

    parlay::sequence<group> point_groups = parlay::tabulate<group>(npg,[&] (size_t i) {return group(i);});
    size_t* pg_asg = new size_t[n]; //store the point-groups each point is assigned to
    if (do_point_groups) {
      LSH<T> lsh_init;
      lsh_init(v,n,d,ad,npg,c,pg_asg,D);
      //TODO unfortunate sequential for loop over n
      for (size_t i = 0; i < n; i++) {
        point_groups[pg_asg[i]].member_ids.push_back(i);
        pts[i].group_id=pg_asg[i];
      }
      //confirm all point groups nonempty
      assert_proper_point_group_size(n,pts,point_groups,npg,false); 
    }
    delete[] pg_asg;

    // TODO will floats overflow (if accidental add to numeric_limits max)?
    //Remark: size of lbs dependent on whether or not we are doing point_groups
    //if we are doing point groups, size npg * t
    //if we aren't, size n * t
    parlay::sequence<parlay::sequence<float>> lbs;
    if (do_point_groups) {
      lbs = parlay::tabulate(npg, [&](size_t i) {
      return parlay::sequence<float>(t, std::numeric_limits<float>::max());
    });
    }
    else {
      lbs = parlay::tabulate(n, [&](size_t i) {
      return parlay::sequence<float>(t, std::numeric_limits<float>::max());
    });
    }

    init_point_bounds(pts,n,d,ad,k,centers,D, lbs,point_groups,npg,do_point_groups);

    assignment_time = tim.next_time();

    // compute num_members for each center
    // TODO which is faster, histogram by key or integer sort type method?
    // center_member_dist stores how many members (points) each center owns
    auto center_member_dist = parlay::histogram_by_key(
       parlay::map(pts, [&](point& p) { return p.best; }));
    parlay::parallel_for(0, k, [&](size_t i) {
      centers[i].has_changed = true;
      centers[i].new_num_members = 0;
      centers[i].old_num_members = 0;
    });

    parlay::parallel_for(0, center_member_dist.size(), [&](size_t i) {
      centers[center_member_dist[i].first].new_num_members =
         center_member_dist[i].second;
      centers[center_member_dist[i].first].old_num_members =
         center_member_dist[i].second;
    });

    assert_members_n(n, k, centers);

    // iters start at 1 as we have already done a closest point check
    size_t iters = 1;
    float max_diff = 0.0;
    // keep track of the number of distance calculations
    parlay::sequence<size_t> distance_calculations(n, k);
    // keep track of the number of centers reassigned in an iteration
    parlay::sequence<int> center_reassignments(n, 1);

    // for center calculation
    CT* new_centers = new CT[k * ad];
    setup_time = tim.next_time();

    while (true) {   // our iteration loop, will stop when we've done max_iter
                     // iters, or if we converge (within epsilon)

      // TODO use yy-style comparative compute_centers in future iterations
      // (once it is actually faster) copying over to c array to use a shared
      // compute_centers function with naive. Copying this should be relatively
      // cheap so not concerned TODO is this actually cheap?
      parlay::parallel_for(0, k, [&](size_t i) {
        for (size_t j = 0; j < d; j++) {
          c[i * ad + j] = centers[i].coordinates[j];
        }
      });
      parlay::parallel_for(0, n, [&](size_t i) { asg[i] = pts[i].best; });
      this->compute_centers(v, n, d, ad, k, c, new_centers, asg);

      parlay::parallel_for(0, k, [&](size_t i) {
        centers[i].delta =
           ptr_dist(centers[i].begin(), new_centers + i * ad, d, D);
      });

      // max_diff is the largest center movement
      max_diff = *parlay::max_element(
         parlay::map(centers, [&](const center& cen) { return cen.delta; }));

      // Copy back over new centers
      parlay::parallel_for(0, k, [&](size_t j) {
        for (size_t coord = 0; coord < d; coord++) {
          centers[j].coordinates[coord] = new_centers[j * ad + coord];
        }
      });

      // for each group, get max drift for group
      parlay::parallel_for(0, t, [&](size_t i) {
        auto drifts = parlay::map(groups[i].member_ids, [&](index_type j) {
          return centers[j].delta;
        });

        groups[i].max_drift = *max_element(drifts);
      });

      // TODO how expensive is this calculation? (will it affect logging time?)
      float msse =
         parlay::reduce(parlay::delayed_tabulate(
            n,
            [&](size_t i) {
              return pc_dist_squared(pts[i], d, centers[pts[i].best], D);
            })) /
         n;

      update_time = tim.next_time();

      // end of iteration stat updating
      if (!suppress_logging) {
        logger.add_iteration(
           iters, assignment_time, update_time, msse,
           parlay::reduce(distance_calculations),
           parlay::reduce(center_reassignments),
           parlay::map(centers, [&](center& cen) { return cen.delta; }),
           setup_time);
      }

      assignment_time = update_time = setup_time = 0;

      // convergence check
      if (iters >= max_iter || max_diff <= epsilon)
        break;

      iters += 1;   // start a new iteration

      for (size_t i = 0; i < distance_calculations.size(); i++)
        distance_calculations[i] = 0;

      for (size_t i = 0; i < center_reassignments.size(); i++)
        center_reassignments[i] = 0;

      if (!suppress_logging) {
        std::cout << "iter: " << iters << std::endl;
      }

      // set centers changed to false, update old_num_members
      parlay::parallel_for(0, k, [&](size_t i) {
        centers[i].has_changed = false;
        centers[i].old_num_members = centers[i].new_num_members;
      });

      assign_step(pts,n,d,ad,k,centers,groups,t,D,lbs,distance_calculations,center_reassignments,point_groups,npg);

      assignment_time=tim.next_time();

      //update center.has_changed, center_reassg
      // mark centers have changed. yes this is a race, but because we
      // are setting false to true and 0 to 1 this is fine       
      parlay::parallel_for(0,n,[&] (size_t i) {
        point& p = pts[i];
        if (p.best != p.old_best) {
          if (center_reassignments[i] != 1)
          center_reassignments[i] = 1;
         
          if (!centers[p.best].has_changed)
            centers[p.best].has_changed = true;
          if (!centers[p.old_best].has_changed)
            centers[p.old_best].has_changed = true;
        }
      });


      // record num_new_members for each center
      auto new_center_member_dist = parlay::histogram_by_key(
         parlay::map(pts, [&](point& p) { return p.best; }));

      parlay::parallel_for(0, new_center_member_dist.size(), [&](size_t i) {
        centers[new_center_member_dist[i].first].new_num_members =
           new_center_member_dist[i].second;
      });

      setup_time = tim.next_time();
    }

    // copy back data
    parlay::parallel_for(0, k, [&](size_t i) {
      for (size_t j = 0; j < d; j++) {
        c[i * ad + j] = centers[i].coordinates[j];
      }
    });
    parlay::parallel_for(0, n, [&](size_t i) { asg[i] = pts[i].best; });

    delete[] new_centers;
  }
  std::string name() { return "yy"; }
};

#endif   // YYIMP